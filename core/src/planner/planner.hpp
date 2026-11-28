// The planner's two passes (stage 3 §4.1): a transcript becomes items, and an
// item becomes one action draft. It only writes drafts: nothing here can send
// or change anything. Transcripts and context are data: whatever they say,
// the model can only fill in the shapes it's given, and what it fills in is
// checked against what it was shown.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "planner/llm/llm.hpp"
#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace micnote::planner {

struct Line {  // a transcript segment
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
};

struct Item {
  std::string kind;  // todo, note, decision or question
  std::string text;  // the model's one-line summary
  std::string said;  // the transcript's own words
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  const ActionType *intent = nullptr;  // null: no action, a plain note
};

struct PlannerOptions {
  // "2026-11-24" and "-05:00": the prompt gets a calendar of the days around
  // today, so the model looks "Thursday" up instead of working it out.
  std::string today;
  std::string utc_offset;
  // Names and jargon from settings.
  std::string vocabulary;
  // A window of transcript, in characters (about 4 to a token).
  std::size_t window_chars = 12000;
  std::size_t overlap_lines = 6;
};

using KeepGoing = std::function<bool()>;

// Pass 1. A marker (ms) always starts a new item. Items come out in order.
std::expected<std::vector<Item>, LlmError> Segment(Llm &llm, const SchemaRegistry &registry,
                                                   std::span<const Line> lines,
                                                   std::span<const std::int64_t> markers,
                                                   const PlannerOptions &options,
                                                   const KeepGoing &keep_going = {});

// Pass 2's prompt for an item with an intent, without the grammar: what a
// local model and Claude are both asked (only context relevant to what was
// said is included).
LlmRequest DraftRequest(const Item &item, const nlohmann::json &context,
                        const PlannerOptions &options, const std::string &instruction = {});

// Pass 2: a draft (docs/action-schema.md §3) for an item with an intent,
// grounded and ready to Validate(). context is what the fetchers returned;
// instruction, the user's own ("make it shorter"), if this is a redo.
std::expected<nlohmann::json, LlmError> Draft(Llm &llm, const Item &item,
                                              const nlohmann::json &context,
                                              const PlannerOptions &options,
                                              const std::string &instruction = {},
                                              const KeepGoing &keep_going = {});

// Removes from a draft what nobody asked for, whatever the model was talked
// into; what that leaves empty joins missing_fields. `said` is the user's own
// words, context what the fetchers returned:
// - an address must have been said, or be one of the emails of a contact
//   (context.contacts) whose name was said;
// - a repository must have been said in full, or be one of context.repos
//   whose own name was said;
// - an issue or pull request number must have been said;
// - an account must be one of context.accounts;
// - a review's line comments need the pull request's diff (context.pr).
// Nothing else in the context counts: a note in a contact's details can't
// add a recipient.
void Ground(nlohmann::json &draft, const ActionType &action, const std::string &said,
            const nlohmann::json &context);

}  // namespace micnote::planner
