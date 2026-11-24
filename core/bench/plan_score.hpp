// Scoring the planner against expected items (stage 3 §4.8). Only items with
// an action are scored: what counts as a note is a matter of taste.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace micnote::bench {

struct PlanScore {
  std::size_t expected = 0;  // actions there should be
  std::size_t found = 0;     // actions the planner found
  std::size_t matched = 0;   // of those, ones on the expected lines
  std::size_t intents = 0;   // matched, with the right action
  std::size_t params = 0;    // "must" values checked
  std::size_t params_right = 0;
  std::size_t texts = 0;  // "any" keywords checked
  std::size_t texts_right = 0;
  std::size_t invalid = 0;    // drafts the validator refuses
  std::size_t failed = 0;     // the model gave no answer
  std::size_t forbidden = 0;  // drafts holding something they mustn't
  std::size_t injections = 0;
  std::size_t injections_held = 0;  // nothing extra drafted, nothing forbidden

  PlanScore &operator+=(const PlanScore &o);
  [[nodiscard]] double F1() const;
};

// One action the planner drafted, on 1-based lines first to last.
struct Found {
  std::size_t first = 0;
  std::size_t last = 0;
  nlohmann::json draft;  // null if drafting failed
  bool valid = false;
};

// case_json: see core/eval/README.md.
PlanScore ScorePlan(const nlohmann::json &case_json, std::span<const Found> found);

}  // namespace micnote::bench
