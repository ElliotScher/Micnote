// The action schema registry (stage 3 §4.2, docs/action-schema.md): which
// actions exist, what each one's params look like, and how risky it is.
// The same definitions drive the local model's grammar, Claude's structured
// outputs, the validator (action_validator.hpp) and the editors.
//
// Params schemas are a small subset of JSON Schema that all of those accept.
// Add() refuses a definition outside it.
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace micnote::planner {

enum class Risk : std::uint8_t { kLow, kMedium, kHigh };
std::string_view ToString(Risk risk);

struct ActionType {
  std::string connector;
  int version = 0;  // the connector's
  std::string type;
  std::string description;
  Risk risk = Risk::kHigh;
  // Read-only lookups the planner may ask for (§4.7).
  std::vector<std::string> context_fetchers;
  nlohmann::json params_schema;
};

class SchemaRegistry {
 public:
  // The v1 connectors (connectors/*.json), as built into the library.
  static std::expected<SchemaRegistry, std::string> Builtin();

  // Adds a connector's definition, replacing an earlier one of the same or a
  // lower version. All or nothing; the error says what's wrong with it.
  std::expected<void, std::string> Add(std::string_view definition_json);

  // Null if there's no such action.
  [[nodiscard]] const ActionType *Find(std::string_view connector, std::string_view type) const;
  // Every action, by connector and then type.
  [[nodiscard]] std::vector<const ActionType *> All() const;

 private:
  std::map<std::pair<std::string, std::string>, ActionType> actions_;
};

// Why `schema` isn't in the supported subset; empty if it is.
std::string CheckSchema(const nlohmann::json &schema);

}  // namespace micnote::planner
