// Turns a schema (the subset schema_registry.hpp accepts) into a llama.cpp
// GBNF grammar, so a local model can only write JSON of that shape (stage 3
// §4.3). Properties come out in a fixed order: required ones, then optional.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace micnote::planner {

// schema must have passed CheckSchema(), except that a property may also have
// "x-nullable": true, which lets the model write null for it.
std::string ToGrammar(const nlohmann::json &schema);

// A quoted GBNF literal for `text`.
std::string GbnfLiteral(std::string_view text);

// The schema a drafting model fills in: the action's params, where a number,
// choice, date or address may be null and text or a list left empty, plus
// rationale and confidence. Nothing else is the model's to write.
nlohmann::json DraftSchema(const nlohmann::json &params_schema);

// The draft (docs/action-schema.md §3) from what the model wrote: blanks are
// dropped, and the required fields left without a value become
// missing_fields. Working that out here rather than asking the model is
// deliberate: small models list fields they filled, and loop. nullopt if the
// output isn't a draft at all. Validate() it next.
std::optional<nlohmann::json> FinishDraft(const ActionType &action, std::string_view model_output);

}  // namespace micnote::planner
