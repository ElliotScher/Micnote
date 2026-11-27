#include "jobs/plan_job.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "planner/action_validator.hpp"
#include "store/repos.hpp"

namespace micnote::jobs {

namespace {

using nlohmann::json;
using Kind = JobError::Kind;

// GCOVR_EXCL_BR_START: as in transcribe_job.cpp, what's uncovered here is
// gcc's exception cleanup; jobs_test.cpp has a case for each outcome.
JobResult Fail(Kind kind, std::string message) {
  return std::unexpected(JobError{.kind = kind, .message = std::move(message)});
}

std::string Two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

// What "today" was when the recording was made, where the user is.
planner::PlannerOptions When(const PlanDeps &deps, std::int64_t unix_ms) {
  using namespace std::chrono;
  const int offset = deps.utc_offset ? deps.utc_offset(unix_ms) : 0;
  const year_month_day d{
      floor<days>(sys_time<milliseconds>(milliseconds(unix_ms) + minutes(offset)))};
  planner::PlannerOptions o;
  o.today = std::to_string(static_cast<int>(d.year())) + "-" +
            Two(static_cast<int>(static_cast<unsigned>(d.month()))) + "-" +
            Two(static_cast<int>(static_cast<unsigned>(d.day())));
  o.utc_offset =
      (offset < 0 ? "-" : "+") + Two(std::abs(offset) / 60) + ":" + Two(std::abs(offset) % 60);
  return o;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

}  // namespace

JobResult RunPlan(JobContext &ctx, const PlanDeps &deps) {
  const std::optional<std::int64_t> ref = ctx.job().ref_id;
  if (!ref) {
    return Fail(Kind::kFatal, "planner.plan needs a recording");
  }
  const std::int64_t id = *ref;
  store::DbResult<void> db_ok;
  std::optional<store::StoredRecording> rec;
  std::optional<store::Transcript> transcript;
  ctx.WithDb([&](store::Db &db) {
    auto r = store::GetRecording(db, id);
    auto t = store::GetTranscript(db, id);
    if (!r || !t) {
      db_ok = std::unexpected(r ? t.error() : r.error());
      return;
    }
    rec = std::move(*r);
    transcript = std::move(*t);
  });
  if (!db_ok) {
    return Fail(Kind::kRetryable, "store: " + db_ok.error().message);
  }
  if (!rec || !transcript) {
    return Fail(Kind::kFatal, "no transcript for recording " + std::to_string(id));
  }
  auto llm = deps.llm();
  if (!llm) {
    return Fail(Kind::kRetryable, "no language model");
  }

  const std::int64_t recorded = rec->start_unix_ms_fixed.value_or(rec->start_unix_ms);
  const planner::PlannerOptions options = When(deps, recorded != 0 ? recorded : ctx.Now());
  std::vector<planner::Line> lines;
  for (const auto &s : transcript->segments) {
    lines.push_back({.start_ms = s.start_ms, .end_ms = s.end_ms, .text = s.text});
  }
  std::vector<std::int64_t> markers;
  for (const auto &m : rec->markers) {
    markers.push_back(m.offset_ms);
  }
  const auto keep_going = [&] { return !ctx.Cancelled(); };
  const auto items = planner::Segment(**llm, *deps.registry, lines, markers, options, keep_going);
  if (!items) {
    return items.error() == planner::LlmError::kCancelled
               ? Fail(Kind::kStopped, "stopped")
               : Fail(Kind::kFatal, "the language model couldn't split the transcript");
  }

  std::vector<store::PlannedItem> plan;
  for (std::size_t i = 0; i < items->size(); ++i) {
    const planner::Item &item = (*items)[i];
    store::PlannedItem p{.kind = item.kind,
                         .text = item.text,
                         .said = item.said,
                         .start_ms = item.start_ms,
                         .end_ms = item.end_ms,
                         .connector = {},
                         .action_type = {},
                         .draft_json = {}};
    if (item.intent != nullptr) {
      const json context = deps.fetch ? deps.fetch(item) : json::object();
      const auto draft = planner::Draft(**llm, item, context, options, {}, keep_going);
      if (!draft && draft.error() == planner::LlmError::kCancelled) {
        return Fail(Kind::kStopped, "stopped");
      }
      // A draft that failed, or that the validator refuses, leaves the item
      // as a note: better no draft than a wrong one.
      if (draft && planner::Validate(*deps.registry, *draft, planner::Stage::kDraft).empty()) {
        p.connector = item.intent->connector;
        p.action_type = item.intent->type;
        p.draft_json = draft->dump(-1, ' ', false, json::error_handler_t::replace);
      }
    }
    plan.push_back(std::move(p));
    ctx.Progress(static_cast<int>((i + 1) * 100 / items->size()));
  }

  bool saved = false;
  ctx.WithDb([&](store::Db &db) {
    auto s = store::SavePlan(db, id, plan, ctx.Now());
    if (!s) {
      db_ok = std::unexpected(s.error());
      return;
    }
    saved = *s;
  });
  if (!db_ok) {
    return Fail(Kind::kRetryable, "store: " + db_ok.error().message);
  }
  if (!saved) {
    return Fail(Kind::kFatal, "this recording's actions have already been worked on");
  }
  ctx.Emit(json{{"type", "items.ready"}, {"recording", id}}.dump());
  return {};
}
// GCOVR_EXCL_BR_STOP

}  // namespace micnote::jobs
