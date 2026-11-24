// A language model the planner can ask (stage 3 §4.3): the local one
// (llama_backend.hpp, in the optional micnote_llm target); tests use a fake.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace micnote::planner {

struct LlmRequest {
  std::string system;
  std::string user;
  std::string grammar;  // GBNF (grammar.hpp); empty for free text
  float temperature = 0.2F;
  int max_tokens = 2048;
  std::uint32_t seed = 0;
};

enum class LlmError : std::uint8_t {
  kModel,      // the model can't be loaded, or its chat format isn't known
  kBackend,    // that compute backend isn't available here
  kTooLong,    // the prompt doesn't fit the context
  kTruncated,  // max_tokens reached before the answer was complete
  kGrammar,    // the grammar doesn't parse
  kFailed,     // the model failed while running
  kCancelled,
};

class Llm {
 public:
  Llm() = default;
  Llm(const Llm &) = delete;
  Llm &operator=(const Llm &) = delete;
  Llm(Llm &&) = delete;
  Llm &operator=(Llm &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is Llm, which is abstract.
  virtual ~Llm() = default;
  // GCOVR_EXCL_STOP

  // The whole answer. `keep_going`, called often, cancels by returning false.
  virtual std::expected<std::string, LlmError> Complete(
      const LlmRequest &request, const std::function<bool()> &keep_going = {}) = 0;
  [[nodiscard]] virtual std::string Model() const = 0;
  [[nodiscard]] virtual std::string Backend() const = 0;
};

}  // namespace micnote::planner
