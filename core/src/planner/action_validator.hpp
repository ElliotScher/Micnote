// Checks an action draft against its schema (docs/action-schema.md §4): the
// params schema, then rules a schema can't express (lengths, an event ending
// after it starts, a review comment on a line the diff has). Nothing here
// trusts the draft: it may be a model's output shaped by injected text.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace micnote::planner {

struct FieldError {
  std::string path;     // a JSON pointer into params ("/to/0"), or "" for the whole draft
  std::string code;     // stable, for programs: "missing", "type", "format", ...
  std::string message;  // for people
};

enum class Stage : std::uint8_t {
  // A planner's draft: a required field may be absent or empty if the draft
  // lists it in missing_fields (the editor then asks for it).
  kDraft,
  // About to be confirmed or executed: everything must be there.
  kFinal,
};

// The lines a review can comment on, by (path, side "LEFT" or "RIGHT").
using DiffLines = std::map<std::pair<std::string, std::string>, std::set<std::int64_t>>;

// draft: {connector, type, schema_version, params, rationale, confidence,
// missing_fields}. With diff, a review's comments must be on its lines.
// Empty if it's valid.
std::vector<FieldError> Validate(const SchemaRegistry &registry, const nlohmann::json &draft,
                                 Stage stage, const DiffLines *diff = nullptr);

// How risky running this is: the registry's, never the draft's own claim. A
// review that only comments is medium.
Risk EffectiveRisk(const ActionType &action, const nlohmann::json &params);

// Seconds since 1970 for an RFC 3339 date-time with an offset; false if it
// isn't one (or isn't a real date).
bool ParseDateTime(std::string_view s, std::int64_t &unix_s);

}  // namespace micnote::planner
