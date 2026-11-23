// The extern "C" entry points (micnote.h). Each one catches everything:
// no C++ exception ever crosses the ABI.
#include <micnote.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "api/context.hpp"

struct mn_context {
  std::unique_ptr<micnote::api::Context> impl;
};

namespace {

// A malloc'd copy, freed by mn_free (so any allocator on the host side is
// irrelevant).
char *Dup(const std::string &s) {
  auto *p = static_cast<char *>(std::malloc(s.size() + 1));  // NOLINT(cppcoreguidelines-no-malloc)
  if (p != nullptr) {  // GCOVR_EXCL_BR_LINE: malloc failing can't be arranged in a test
    std::memcpy(p, s.c_str(), s.size() + 1);
  }
  return p;
}

constexpr const char *kNeedsCtxAndMethod =
    R"({"error":{"code":"invalid_args","message":"ctx and method are required"}})";

}  // namespace

extern "C" uint32_t mn_abi_version(void) { return MN_ABI_VERSION; }

extern "C" mn_status mn_context_create(const mn_config *config, mn_context **out) {
  if (out == nullptr) {
    return MN_E_INVALID;
  }
  *out = nullptr;
  if (config == nullptr) {
    return MN_E_INVALID;
  }
  // GCOVR_EXCL_BR_START: the exception paths here are out-of-memory only.
  try {
    auto ctx = micnote::api::Context::Open(*config);
    if (!ctx) {
      return ctx.error().status;
    }
    *out = new mn_context{.impl = std::move(*ctx)};  // NOLINT(cppcoreguidelines-owning-memory)
    return MN_OK;
  } catch (const std::exception &) {  // GCOVR_EXCL_LINE: only out of memory
    return MN_E_INTERNAL;             // GCOVR_EXCL_LINE
  }  // GCOVR_EXCL_LINE
  // GCOVR_EXCL_BR_STOP
}

extern "C" void mn_context_destroy(mn_context *ctx) {
  delete ctx;  // NOLINT(cppcoreguidelines-owning-memory)
}

extern "C" mn_status mn_call(mn_context *ctx, const char *method, const char *params_json,
                             char **result_json) {
  if (result_json == nullptr) {
    return MN_E_INVALID;
  }
  *result_json = nullptr;
  if (ctx == nullptr || method == nullptr) {
    *result_json = Dup(kNeedsCtxAndMethod);
    return MN_E_INVALID;
  }
  const auto r = ctx->impl->Call(method, params_json != nullptr ? params_json : "");
  *result_json = Dup(r.json);
  return *result_json != nullptr ? r.status : MN_E_INTERNAL;  // GCOVR_EXCL_BR_LINE: see Dup
}

extern "C" mn_status mn_submit(mn_context *ctx, const char *method, const char *params_json,
                               uint64_t *job_id, char **error_json) {
  if (error_json != nullptr) {
    *error_json = nullptr;
  }
  if (job_id == nullptr || ctx == nullptr || method == nullptr) {
    if (job_id != nullptr) {
      *job_id = 0;
    }
    if (error_json != nullptr) {
      *error_json = Dup(kNeedsCtxAndMethod);
    }
    return MN_E_INVALID;
  }
  // GCOVR_EXCL_BR_START: the exception paths here are out-of-memory only.
  try {
    const auto r = ctx->impl->Submit(method, params_json != nullptr ? params_json : "");
    *job_id = r.job;
    if (r.status != MN_OK && error_json != nullptr) {
      *error_json = Dup(r.error_json);
    }
    return r.status;
  } catch (const std::exception &) {  // GCOVR_EXCL_LINE: only out of memory
    *job_id = 0;                      // GCOVR_EXCL_LINE
    return MN_E_INTERNAL;             // GCOVR_EXCL_LINE
  }  // GCOVR_EXCL_LINE
  // GCOVR_EXCL_BR_STOP
}

extern "C" mn_status mn_cancel(mn_context *ctx, uint64_t job_id) {
  return ctx != nullptr ? ctx->impl->Cancel(job_id) : MN_E_INVALID;
}

extern "C" void mn_free(void *p) {
  std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}
