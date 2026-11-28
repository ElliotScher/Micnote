#include "planner/cloud.hpp"

#include <algorithm>
#include <span>

#include "crypto/psa.hpp"
#include "planner/grammar.hpp"

namespace micnote::planner {

namespace {

using nlohmann::json;

constexpr int kMostTokens = 64000;

std::string Hex(std::span<const std::byte> bytes) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (const std::byte b : bytes) {
    out += kDigits[std::to_integer<unsigned>(b) >> 4U];
    out += kDigits[std::to_integer<unsigned>(b) & 0xFU];
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

// The models with no server-side fallback, which a "fallbacks" field
// can't help (an explicit list would be refused).
bool HasFallbacks(std::string_view model) { return !model.starts_with("claude-haiku"); }

std::string Dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace

// GCOVR_EXCL_BR_START: from here, what gcov reports uncovered is gcc's
// exception cleanup for strings and json temporaries; cloud_test.cpp has a
// case for each rule, and line coverage still applies.

json ClaudeSchema(const json &schema) {
  json out = json::object();
  for (const char *key : {"type", "description", "enum", "required", "additionalProperties"}) {
    if (const auto it = schema.find(key); it != schema.end()) {
      out[key] = *it;
    }
  }
  if (const auto it = schema.find("properties"); it != schema.end()) {
    out["properties"] = json::object();
    for (const auto &[name, sub] : it->items()) {
      out["properties"][name] = ClaudeSchema(sub);
    }
  }
  if (const auto it = schema.find("items"); it != schema.end()) {
    out["items"] = ClaudeSchema(*it);
  }
  if (schema.value("x-nullable", false)) {
    return {{"anyOf", {std::move(out), {{"type", "null"}}}}};
  }
  return out;
}

CloudRequest BuildCloudRequest(const Item &item, const json &context, const PlannerOptions &options,
                               const CloudSettings &settings, const std::string &instruction) {
  CloudRequest request{.body = {}, .hash = {}, .redactor = Redactor(settings.redact)};
  const LlmRequest prompt = DraftRequest(item, context, options, instruction);
  json body = {
      {"model", settings.model},
      {"max_tokens", settings.max_tokens},
      {"output_config",
       {{"effort", settings.effort},
        {"format",
         {{"type", "json_schema"},
          {"schema", ClaudeSchema(DraftSchema(item.intent->params_schema))}}}}},
      {"system", request.redactor.Redact(prompt.system)},
      {"messages", {{{"role", "user"}, {"content", request.redactor.Redact(prompt.user)}}}}};
  if (HasFallbacks(settings.model)) {
    body["fallbacks"] = "default";
  }
  request.body = Dump(body);
  request.hash = Hex(crypto::Sha256(std::as_bytes(std::span(request.body))));
  return request;
}

std::string WithMoreTokens(const std::string &body) {
  json b = json::parse(body);
  b["max_tokens"] = std::min(b.value("max_tokens", 0) * 2, kMostTokens);
  return Dump(b);
}

std::optional<json> FinishCloudDraft(const CloudRequest &request, const Item &item,
                                     const json &context, const std::string &instruction,
                                     std::string_view answer) {
  auto draft = FinishDraft(*item.intent, answer);
  if (!draft) {
    return std::nullopt;
  }
  (*draft)["params"] = request.redactor.RestoreJson((*draft)["params"]);
  (*draft)["rationale"] = request.redactor.RestoreJson((*draft)["rationale"]);
  Ground(*draft, *item.intent, item.said + " . " + instruction, context);
  return draft;
}

std::expected<CloudDraftResult, ClaudeError> CloudDraft(const ClaudeClient &client,
                                                        const CloudRequest &request,
                                                        const Item &item, const json &context,
                                                        const std::string &instruction,
                                                        const std::function<bool()> &keep_going) {
  auto reply = SendToClaude(client, request.body, keep_going);
  if (!reply && reply.error().kind == ClaudeFailure::kTruncated) {
    reply = SendToClaude(client, WithMoreTokens(request.body), keep_going);
  }
  if (!reply) {
    return std::unexpected(reply.error());
  }
  auto draft = FinishCloudDraft(request, item, context, instruction, reply->text);
  if (!draft) {
    return std::unexpected(
        ClaudeError{.kind = ClaudeFailure::kBadResponse, .message = "the answer isn't a draft"});
  }
  return CloudDraftResult{.draft = std::move(*draft), .reply = std::move(*reply)};
}

// GCOVR_EXCL_BR_STOP

}  // namespace micnote::planner
