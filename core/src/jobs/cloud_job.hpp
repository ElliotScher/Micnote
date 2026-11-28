// Drafting one item with Claude (stage 3 §4.5, §4.6), in two steps the user
// sees:
// - PrepareCloud() makes the exact request for an item: the app shows it
//   (planner.cloud_preview), with its hash;
// - the cloud_draft job ("planner.cloud_draft@1", ref_id = the item, params
//   {"preview_hash"}) makes it again, and sends it only if the hash is the
//   one the user saw. Claude's draft is restored, grounded and validated
//   like a local one, and becomes the item's next revision (author
//   planner_cloud). Every call is audited, without its content.
// What's sent is shaped by settings: planner.cloud.model (a model id;
// claude-opus-5-5 when unset), planner.redact.terms (a list of words to
// hide) and planner.redact.names (true: hide contacts' names too).
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "http/http.hpp"
#include "jobs/queue.hpp"
#include "planner/cloud.hpp"
#include "planner/schema_registry.hpp"
#include "secrets/secrets.hpp"
#include <nlohmann/json.hpp>

namespace micnote::jobs {

// Where the user's Claude API key is kept (secrets.hpp).
inline constexpr std::string_view kClaudeKeySecret = "anthropic_api_key";

struct CloudDeps {
  const planner::SchemaRegistry *registry = nullptr;
  // As PlanDeps's: what's known about an item, and where the user is.
  std::function<nlohmann::json(const planner::Item &)> fetch;
  std::function<int(std::int64_t unix_ms)> utc_offset;
  // How requests go out; without one, nothing can be sent (a build without
  // HTTPS), though a preview can still be made.
  std::shared_ptr<http::Client> http;
  std::string base_url = "https://api.anthropic.com";
  secrets::Secrets *secrets = nullptr;
  // Before a retry (claude_backend.hpp); tests skip the wait.
  std::function<bool(std::chrono::milliseconds)> wait;
};

struct PreparedCloud {
  planner::Item item;
  nlohmann::json context;
  planner::CloudRequest request;
  std::string model;
};

struct PrepareError {
  enum class Kind : std::uint8_t {
    kNotFound,  // no such item
    kNoAction,  // a note: nothing to draft
    kStore,
  };
  Kind kind = Kind::kStore;
  std::string message;
};

std::expected<PreparedCloud, PrepareError> PrepareCloud(store::Db &db, std::int64_t item,
                                                        const CloudDeps &deps);

JobResult RunCloudDraft(JobContext &ctx, const CloudDeps &deps);

}  // namespace micnote::jobs
