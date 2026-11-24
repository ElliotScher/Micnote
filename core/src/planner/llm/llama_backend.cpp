#include "planner/llm/llama_backend.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <ggml-backend.h>
#include <llama.h>

namespace micnote::planner {

namespace {

void Quiet(enum ggml_log_level /*level*/, const char * /*text*/, void * /*user_data*/) {}

void InitOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    llama_log_set(Quiet, nullptr);
    llama_backend_init();
  });
}

// The first GPU's backend ("vulkan"), or "" without one.
std::string GpuName() {
  for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t dev = ggml_backend_dev_get(i);
    const auto type = ggml_backend_dev_type(dev);
    if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
      std::string name = ggml_backend_reg_name(ggml_backend_dev_backend_reg(dev));
      std::ranges::transform(name, name.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return name;
    }
  }
  return {};
}

struct SamplerFree {
  void operator()(llama_sampler *s) const { llama_sampler_free(s); }
};
using Sampler = std::unique_ptr<llama_sampler, SamplerFree>;

}  // namespace

std::expected<std::unique_ptr<LlamaLlm>, LlmError> LlamaLlm::Load(
    const std::filesystem::path &model, std::string_view backend, int context_tokens) {
  InitOnce();
  if (backend != "auto" && backend != "cpu") {
    return std::unexpected(LlmError::kBackend);
  }
  const std::string gpu = backend == "cpu" ? std::string() : GpuName();
  llama_model_params mp = llama_model_default_params();
  mp.n_gpu_layers = gpu.empty() ? 0 : 999;
  // An empty device list keeps every part of the work off the GPU.
  std::array<ggml_backend_dev_t, 1> none{nullptr};
  if (gpu.empty()) {
    mp.devices = none.data();
  }
  llama_model *m = llama_model_load_from_file(model.string().c_str(), mp);
  if (m == nullptr) {
    return std::unexpected(LlmError::kModel);
  }
  llama_context_params cp = llama_context_default_params();
  cp.n_ctx = static_cast<std::uint32_t>(context_tokens);
  cp.n_batch = 512;
  cp.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
  cp.n_threads_batch = cp.n_threads;
  llama_context *ctx = llama_init_from_model(m, cp);
  if (ctx == nullptr) {
    llama_model_free(m);
    return std::unexpected(LlmError::kModel);
  }
  return std::unique_ptr<LlamaLlm>(
      new LlamaLlm(m, ctx, model.stem().string(), gpu.empty() ? "cpu" : gpu));
}

LlamaLlm::~LlamaLlm() {
  llama_free(ctx_);
  llama_model_free(model_);
}

std::expected<std::vector<int>, LlmError> LlamaLlm::Prompt(const LlmRequest &request) const {
  // The chat template is laid out around two stand-ins, and only the
  // template's own text may be special tokens. What goes in their place is
  // tokenized as plain text: a transcript that says "<|im_end|>" must not be
  // able to end the user's turn.
  constexpr const char *kSystem = "MICNOTE-SYSTEM-7c1f";
  constexpr const char *kUser = "MICNOTE-USER-93ab";
  const char *tmpl = llama_model_chat_template(model_, nullptr);
  if (tmpl == nullptr) {
    return std::unexpected(LlmError::kModel);
  }
  const std::array<llama_chat_message, 2> chat = {
      llama_chat_message{.role = "system", .content = kSystem},
      llama_chat_message{.role = "user", .content = kUser}};
  std::string layout(4096, '\0');
  const int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, layout.data(),
                                          static_cast<int>(layout.size()));
  if (n < 0 || std::cmp_greater(n, layout.size())) {
    return std::unexpected(LlmError::kModel);
  }
  layout.resize(static_cast<std::size_t>(n));
  const auto system_at = layout.find(kSystem);
  const auto user_at = layout.find(kUser);
  if (system_at == std::string::npos || user_at == std::string::npos || user_at < system_at) {
    return std::unexpected(LlmError::kModel);
  }
  const std::size_t after_system = system_at + std::string_view(kSystem).size();
  const std::size_t after_user = user_at + std::string_view(kUser).size();

  std::vector<int> out;
  const llama_vocab *vocab = llama_model_get_vocab(model_);
  const auto add = [&](std::string_view text, bool special) {
    std::vector<llama_token> t(text.size() + 8);
    const int count = llama_tokenize(vocab, text.data(), static_cast<int>(text.size()), t.data(),
                                     static_cast<int>(t.size()), out.empty(), special);
    out.insert(out.end(), t.begin(), t.begin() + std::max(count, 0));
    return count >= 0;
  };
  const std::string_view l = layout;
  if (!add(l.substr(0, system_at), true) || !add(request.system, false) ||
      !add(l.substr(after_system, user_at - after_system), true) || !add(request.user, false) ||
      !add(l.substr(after_user), true)) {
    return std::unexpected(LlmError::kFailed);
  }
  return out;
}

std::expected<std::string, LlmError> LlamaLlm::Complete(const LlmRequest &request,
                                                        const std::function<bool()> &keep_going) {
  const llama_vocab *vocab = llama_model_get_vocab(model_);
  auto prompt = Prompt(request);
  if (!prompt) {
    return std::unexpected(prompt.error());
  }
  std::vector<llama_token> &tokens = *prompt;
  if (tokens.empty() ||
      std::cmp_greater_equal(tokens.size() + static_cast<std::size_t>(request.max_tokens),
                             llama_n_ctx(ctx_))) {
    return std::unexpected(LlmError::kTooLong);
  }

  const Sampler chain(llama_sampler_chain_init(llama_sampler_chain_default_params()));
  if (!request.grammar.empty()) {
    llama_sampler *g = llama_sampler_init_grammar(vocab, request.grammar.c_str(), "root");
    if (g == nullptr) {
      return std::unexpected(LlmError::kGrammar);
    }
    llama_sampler_chain_add(chain.get(), g);
  }
  if (request.temperature <= 0) {
    llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
  } else {
    llama_sampler_chain_add(chain.get(), llama_sampler_init_top_p(0.95F, 1));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_temp(request.temperature));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(request.seed));
  }

  llama_memory_clear(llama_get_memory(ctx_), true);
  constexpr std::size_t kBatch = 512;
  for (std::size_t at = 0; at < tokens.size(); at += kBatch) {
    if (keep_going && !keep_going()) {
      return std::unexpected(LlmError::kCancelled);
    }
    const std::size_t count = std::min(kBatch, tokens.size() - at);
    if (llama_decode(ctx_, llama_batch_get_one(&tokens[at], static_cast<int>(count))) != 0) {
      return std::unexpected(LlmError::kFailed);
    }
  }
  std::string out;
  for (int i = 0; i < request.max_tokens; ++i) {
    llama_token token = llama_sampler_sample(chain.get(), ctx_, -1);
    if (llama_vocab_is_eog(vocab, token)) {
      return out;
    }
    std::array<char, 256> piece{};
    const int len =
        llama_token_to_piece(vocab, token, piece.data(), static_cast<int>(piece.size()), 0, false);
    if (len < 0) {
      return std::unexpected(LlmError::kFailed);
    }
    out.append(piece.data(), static_cast<std::size_t>(len));
    if (keep_going && !keep_going()) {
      return std::unexpected(LlmError::kCancelled);
    }
    if (llama_decode(ctx_, llama_batch_get_one(&token, 1)) != 0) {
      return std::unexpected(LlmError::kFailed);
    }
  }
  return std::unexpected(LlmError::kTruncated);
}

}  // namespace micnote::planner
