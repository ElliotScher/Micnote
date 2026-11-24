// The local language model, through llama.cpp, in the optional micnote_llm
// target (-DMICNOTE_LLM=ON, with MICNOTE_ASR: the two share one ggml).
// Complete() isn't reentrant.
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "planner/llm/llm.hpp"

struct llama_model;
struct llama_context;

namespace micnote::planner {

class LlamaLlm final : public Llm {
 public:
  // Loads a GGUF model. backend: "auto" (a GPU if there is one), or "cpu".
  static std::expected<std::unique_ptr<LlamaLlm>, LlmError> Load(const std::filesystem::path &model,
                                                                 std::string_view backend = "auto",
                                                                 int context_tokens = 8192);

  LlamaLlm(const LlamaLlm &) = delete;
  LlamaLlm &operator=(const LlamaLlm &) = delete;
  LlamaLlm(LlamaLlm &&) = delete;
  LlamaLlm &operator=(LlamaLlm &&) = delete;
  ~LlamaLlm() override;

  std::expected<std::string, LlmError> Complete(const LlmRequest &request,
                                                const std::function<bool()> &keep_going) override;
  [[nodiscard]] std::string Model() const override { return name_; }
  [[nodiscard]] std::string Backend() const override { return backend_; }

  // The request as tokens, in the model's chat format; the contents are
  // always plain text.
  [[nodiscard]] std::expected<std::vector<int>, LlmError> Prompt(const LlmRequest &request) const;

 private:
  LlamaLlm(llama_model *model, llama_context *ctx, std::string name, std::string backend)
      : model_(model), ctx_(ctx), name_(std::move(name)), backend_(std::move(backend)) {}

  llama_model *model_;
  llama_context *ctx_;
  std::string name_;
  std::string backend_;
};

}  // namespace micnote::planner
