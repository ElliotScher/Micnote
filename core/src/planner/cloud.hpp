// Drafting an item with Claude instead of the local model (stage 3 §4.5,
// §4.6), only when the user asks, and only with what they previewed:
// - BuildCloudRequest() makes the exact /v1/messages body: the local
//   drafter's prompt, redacted, with the draft's schema as a structured
//   output, so Claude can only answer with JSON of that shape. Its SHA-256
//   is the preview hash: the same item, context and settings always give
//   the same body and hash, so a job can check that what it would send is
//   what the user saw.
// - CloudDraft() sends it, and turns the answer into a draft: placeholders
//   back to the originals, then grounded against what was really said and
//   the real context, like a local draft. Validate() it next.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "planner/llm/claude_backend.hpp"
#include "planner/planner.hpp"
#include "planner/redact.hpp"
#include <nlohmann/json.hpp>

namespace micnote::planner {

struct CloudSettings {
  std::string model{kClaudeDefaultModel};
  // This model thinks at every effort; medium is its default and plenty for
  // one draft.
  std::string effort = "medium";
  int max_tokens = 16000;
  RedactOptions redact;
};

struct CloudRequest {
  std::string body;  // what's sent, byte for byte
  std::string hash;  // its SHA-256, lower-case hex
  Redactor redactor{{}};
};

// A schema the API's structured outputs accept, from DraftSchema()'s: a
// nullable property becomes anyOf [it, null]; "format" goes, because a
// placeholder like [EMAIL_1] must fit where an address goes (the
// validator checks formats once the originals are back); keywords besides
// type, description, enum, properties, required, items and
// additionalProperties go too.
nlohmann::json ClaudeSchema(const nlohmann::json &schema);

// item must have an intent.
CloudRequest BuildCloudRequest(const Item &item, const nlohmann::json &context,
                               const PlannerOptions &options, const CloudSettings &settings,
                               const std::string &instruction = {});

// The same body with room for twice the tokens, at most 64000: for one more
// try after an answer was cut off.
std::string WithMoreTokens(const std::string &body);

// The draft from Claude's answer (the JSON its text blocks hold), or nullopt
// if it isn't one.
std::optional<nlohmann::json> FinishCloudDraft(const CloudRequest &request, const Item &item,
                                               const nlohmann::json &context,
                                               const std::string &instruction,
                                               std::string_view answer);

struct CloudDraftResult {
  nlohmann::json draft;
  ClaudeReply reply;
};

// Sends request, trying once more with WithMoreTokens() if the answer was
// cut off. kBadResponse if the answer isn't a draft.
std::expected<CloudDraftResult, ClaudeError> CloudDraft(
    const ClaudeClient &client, const CloudRequest &request, const Item &item,
    const nlohmann::json &context, const std::string &instruction = {},
    const std::function<bool()> &keep_going = {});

}  // namespace micnote::planner
