#include "jobs/cloud_job.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jobs/plan_job.hpp"
#include "planner/action_validator.hpp"
#include "store/repos.hpp"

namespace micnote::jobs {

namespace {

using nlohmann::json;
using Kind = JobError::Kind;

// GCOVR_EXCL_BR_START: as in plan_job.cpp, what's uncovered here is gcc's
// exception cleanup; jobs_test.cpp has a case for each outcome.
JobResult Fail(Kind kind, std::string message) {
  return std::unexpected(JobError{.kind = kind, .message = std::move(message)});
}

std::unexpected<PrepareError> Refused(PrepareError::Kind kind, std::string message) {
  return std::unexpected(PrepareError{.kind = kind, .message = std::move(message)});
}

// A setting's JSON value; null when unset or unreadable.
store::DbResult<json> Setting(store::Db &db, const std::string &key) {
  auto v = store::GetSetting(db, key);
  if (!v) {
    return std::unexpected(v.error());
  }
  return *v ? json::parse(**v, nullptr, false) : json();
}

std::vector<std::string> Strings(const json &list) {
  std::vector<std::string> out;
  if (list.is_array()) {
    for (const json &s : list) {
      if (s.is_string()) {
        out.push_back(s.get<std::string>());
      }
    }
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

}  // namespace

std::expected<PreparedCloud, PrepareError> PrepareCloud(store::Db &db, std::int64_t item_id,
                                                        const CloudDeps &deps) {
  const auto stored = store::GetItem(db, item_id);
  if (!stored) {
    return Refused(PrepareError::Kind::kStore, stored.error().message);
  }
  if (!*stored) {
    return Refused(PrepareError::Kind::kNotFound, "no item " + std::to_string(item_id));
  }
  const store::StoredItem &s = **stored;
  const std::size_t dot = s.intent.find('.');
  const planner::ActionType *action =
      dot == std::string::npos
          ? nullptr
          : deps.registry->Find(s.intent.substr(0, dot), s.intent.substr(dot + 1));
  if (action == nullptr) {
    return Refused(PrepareError::Kind::kNoAction,
                   "item " + std::to_string(item_id) + " is a note: there's nothing to draft");
  }
  const auto rec = store::GetRecording(db, s.recording);
  const auto model = Setting(db, "planner.cloud.model");
  const auto terms = Setting(db, "planner.redact.terms");
  const auto names = Setting(db, "planner.redact.names");
  for (const store::DbResult<json> *setting : {&model, &terms, &names}) {
    if (!*setting) {
      return Refused(PrepareError::Kind::kStore, setting->error().message);
    }
  }
  if (!rec) {
    return Refused(PrepareError::Kind::kStore, rec.error().message);
  }

  PreparedCloud p{.item = planner::Item{.kind = s.kind,
                                        .text = s.text,
                                        .said = s.said,
                                        .start_ms = s.start_ms,
                                        .end_ms = s.end_ms,
                                        .intent = action},
                  .context = json::object(),
                  .request = {},
                  .model = {}};
  p.context = deps.fetch ? deps.fetch(p.item) : json::object();
  // As the plan job: the dates the memo meant, where it was recorded. (An
  // item outlives nothing but its recording, which cascades it away.)
  const std::int64_t recorded =
      *rec ? (*rec)->start_unix_ms_fixed.value_or((*rec)->start_unix_ms) : 0;
  const planner::PlannerOptions options = PlanOptionsAt(deps.utc_offset, recorded);

  planner::CloudSettings settings;
  if (model->is_string() && !model->get<std::string>().empty()) {
    settings.model = model->get<std::string>();
  }
  settings.redact.terms = Strings(*terms);
  if (names->is_boolean() && names->get<bool>() && p.context.contains("contacts")) {
    for (const json &c : p.context["contacts"]) {
      if (c.is_object() && c.contains("name") && c["name"].is_string()) {
        settings.redact.names.push_back(c["name"].get<std::string>());
      }
    }
  }
  p.model = settings.model;
  p.request = planner::BuildCloudRequest(p.item, p.context, options, settings);
  return p;
}

JobResult RunCloudDraft(JobContext &ctx, const CloudDeps &deps) {
  const std::optional<std::int64_t> ref = ctx.job().ref_id;
  const json params = json::parse(ctx.job().params_json, nullptr, false);
  const auto hash = params.is_object() ? params.find("preview_hash") : params.end();
  if (!ref || !params.is_object() || hash == params.end() || !hash->is_string()) {
    return Fail(Kind::kFatal, "planner.cloud_draft needs an item and its preview_hash");
  }
  if (!deps.http) {
    return Fail(Kind::kFatal, "this build can't reach the Claude API");
  }
  std::expected<PreparedCloud, PrepareError> prepared = std::unexpected(PrepareError{});
  ctx.WithDb([&](store::Db &db) { prepared = PrepareCloud(db, *ref, deps); });
  if (!prepared) {
    const PrepareError &e = prepared.error();
    return Fail(e.kind == PrepareError::Kind::kStore ? Kind::kRetryable : Kind::kFatal, e.message);
  }
  const PreparedCloud &p = *prepared;
  // The user approved these exact bytes; anything else (a contact or a
  // setting changed since) needs a new look.
  if (p.request.hash != hash->get<std::string>()) {
    return Fail(Kind::kFatal, "what would be sent has changed since the preview: preview it again");
  }
  const auto key = deps.secrets != nullptr
                       ? deps.secrets->Get(kClaudeKeySecret)
                       : std::expected<std::vector<std::byte>, secrets::SecretError>(
                             std::unexpected(secrets::SecretError::kNotFound));
  if (!key || key->empty()) {
    return Fail(Kind::kFatal, "no Claude API key: add yours in settings");
  }
  const planner::ClaudeClient client{
      .http = deps.http.get(),
      .api_key = std::string(reinterpret_cast<const char *>(key->data()), key->size()),
      .base_url = deps.base_url,
      .max_retries = 2,
      .wait = deps.wait};
  const auto keep_going = [&] { return !ctx.Cancelled(); };
  const auto result = planner::CloudDraft(client, p.request, p.item, p.context, {}, keep_going);
  if (!result && result.error().kind == planner::ClaudeFailure::kCancelled) {
    return Fail(Kind::kStopped, "stopped");
  }

  json audit = {{"item", *ref}, {"preview_hash", p.request.hash}, {"model", p.model}};
  std::optional<JobResult> outcome;
  std::optional<std::int64_t> action;
  if (!result) {
    audit["outcome"] = planner::ToString(result.error().kind);
    outcome = Fail(result.error().kind == planner::ClaudeFailure::kUnavailable ? Kind::kRetryable
                                                                               : Kind::kFatal,
                   result.error().message);
  } else {
    audit["answered_by"] = result->reply.model;
    audit["fell_back"] = result->reply.fell_back;
    audit["input_tokens"] = result->reply.input_tokens;
    audit["output_tokens"] = result->reply.output_tokens;
    if (!planner::Validate(*deps.registry, result->draft, planner::Stage::kDraft).empty()) {
      audit["outcome"] = "invalid";
      outcome = Fail(Kind::kFatal, "Claude's draft doesn't fit the action");
    }
  }
  store::DbResult<void> db_ok;
  ctx.WithDb([&](store::Db &db) {
    if (!outcome) {
      auto a = store::AddDraftRevision(
          db, *ref, p.item.intent->connector, p.item.intent->type, "planner_cloud",
          result->draft.dump(-1, ' ', false, json::error_handler_t::replace), ctx.Now());
      if (!a) {
        db_ok = std::unexpected(a.error());
        return;
      }
      action = *a;
      audit["outcome"] = action ? "drafted" : "kept";
      audit["action"] = action ? json(*action) : json();
    }
    if (auto logged = store::AddAudit(db, ctx.Now(), "planner_cloud", "cloud.draft", audit.dump());
        !logged) {
      db_ok = std::unexpected(logged.error());
    }
  });
  if (outcome) {
    return *outcome;
  }
  if (!db_ok) {
    // Not retried: that would send (and bill) the request again, which the
    // user approved once.
    return Fail(Kind::kFatal, "can't store Claude's draft: " + db_ok.error().message);
  }
  if (!action) {
    return Fail(Kind::kFatal, "this item's action has been confirmed or discarded since");
  }
  ctx.Emit(json{
      {"type", "action.drafted"}, {"action", *action}, {"item", *ref}, {"author", "planner_cloud"}}
               .dump());
  return {};
}
// GCOVR_EXCL_BR_STOP

}  // namespace micnote::jobs
