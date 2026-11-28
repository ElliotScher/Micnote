// Sending a request to the Claude API and reading the answer (stage 3 §4.6):
// POST /v1/messages over raw HTTPS, since C++ has no official Anthropic SDK.
// The request body itself is built by cloud.hpp, so that what's sent is
// exactly what the user previewed. Checked against the API as of 2026-11:
// - headers: x-api-key, anthropic-version 2026-10-21 (which rejects
//   temperature, top_p and top_k), and anthropic-beta
//   server-side-fallback-2026-07-01 when the body asks for "fallbacks";
// - an answer is HTTP 200 whose stop_reason must be checked before its
//   content: "refusal" (the whole fallback chain declined) and "max_tokens"
//   (cut off) carry no usable JSON;
// - 429, 5xx (529 is "overloaded") and network failures are retried, waiting
//   as retry-after says; other 4xx are not.
// Nothing here logs the request, the response or the key.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "http/http.hpp"
#include <nlohmann/json.hpp>

namespace micnote::planner {

inline constexpr std::string_view kClaudeApiVersion = "2026-10-21";
inline constexpr std::string_view kClaudeFallbackBeta = "server-side-fallback-2026-07-01";
inline constexpr std::string_view kClaudeDefaultModel = "claude-opus-5-5";

struct ClaudeClient {
  http::Client *http = nullptr;
  std::string api_key;
  std::string base_url = "https://api.anthropic.com";
  // Tries after the first, for 429, 5xx and network failures.
  int max_retries = 2;
  // Waits before a retry; false if cancelled meanwhile. Empty: sleeps,
  // checking keep_going every 100 ms.
  std::function<bool(std::chrono::milliseconds)> wait;
};

struct ClaudeReply {
  std::string text;   // the text blocks, joined: the JSON the schema shapes
  std::string model;  // which model answered (a fallback's, if it took over)
  bool fell_back = false;
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
};

enum class ClaudeFailure : std::uint8_t {
  kRefused,      // declined, by every model the fallbacks allowed
  kTruncated,    // max_tokens reached
  kBadRequest,   // 400, 404, 413: the request is wrong, retrying won't help
  kAuth,         // 401, 402, 403: the key, billing or permissions
  kUnavailable,  // 429, 5xx or the network, still failing after the retries
  kBadResponse,  // not the JSON the API documents
  kCancelled,
};

struct ClaudeError {
  ClaudeFailure kind = ClaudeFailure::kBadResponse;
  // For the user: the API's own message, a refusal's category, or what failed.
  std::string message;
};

// Sends `body` (a /v1/messages request) as it is.
std::expected<ClaudeReply, ClaudeError> SendToClaude(const ClaudeClient &client,
                                                     const std::string &body,
                                                     const std::function<bool()> &keep_going = {});

// Reads a 200 response's body. Exposed for the tests.
std::expected<ClaudeReply, ClaudeError> ParseClaudeReply(std::string_view body);

const char *ToString(ClaudeFailure f);

}  // namespace micnote::planner
