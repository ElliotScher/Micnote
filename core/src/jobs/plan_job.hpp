// The plan job ("planner.plan@1", ref_id = the recording): splits a
// recording's transcript into items, drafts an action for each that has one,
// and stores them. Only drafts: nothing is sent or changed. The language
// model comes from a factory the host provides; a library without one never
// queues this job.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

#include "jobs/queue.hpp"
#include "planner/planner.hpp"
#include <nlohmann/json.hpp>

namespace micnote::jobs {

using LlmFactory = std::function<std::expected<std::shared_ptr<planner::Llm>, planner::LlmError>()>;

struct PlanDeps {
  LlmFactory llm;
  const planner::SchemaRegistry *registry = nullptr;
  // The read-only lookups (§4.7): what's known that could help with an item,
  // as {"contacts": [...], "repos": [...], ...}. Empty: nothing is known.
  std::function<nlohmann::json(const planner::Item &)> fetch;
  // Minutes east of UTC at a time (unix ms); UTC when empty.
  std::function<int(std::int64_t unix_ms)> utc_offset;
};

JobResult RunPlan(JobContext &ctx, const PlanDeps &deps);

// What "today" was at a time (unix ms), where the user was then: the
// planner's dates are worked out from it.
planner::PlannerOptions PlanOptionsAt(const std::function<int(std::int64_t)> &utc_offset,
                                      std::int64_t unix_ms);

}  // namespace micnote::jobs
