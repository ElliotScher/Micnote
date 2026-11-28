#include "planner/llm/claude_backend.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <thread>

namespace micnote::planner {

namespace {

using nlohmann::json;
using std::chrono::milliseconds;

constexpr milliseconds kMaxWait{60000};

std::unexpected<ClaudeError> Failed(ClaudeFailure kind, std::string message) {
  return std::unexpected(ClaudeError{.kind = kind, .message = std::move(message)});
}

const std::string *Text(const json &object, const char *key) {
  const auto it = object.find(key);
  return it != object.end() ? it->get_ptr<const std::string *>() : nullptr;
}

std::int64_t Count(const json &object, const char *key) {
  const auto it = object.find(key);
  const auto *n = it != object.end() ? it->get_ptr<const json::number_unsigned_t *>() : nullptr;
  return n != nullptr ? static_cast<std::int64_t>(*n) : 0;
}

// The API's message for an error response, or "HTTP 503".
std::string ApiMessage(const std::string &body, long status) {
  const json r = json::parse(body, nullptr, false);
  if (r.is_object()) {
    const auto e = r.find("error");
    if (e != r.end() && e->is_object()) {
      if (const std::string *m = Text(*e, "message"); m != nullptr) {
        return *m;
      }
    }
  }
  return "HTTP " + std::to_string(status);
}

bool Retryable(long status) {
  return status == 408 || status == 409 || status == 429 || status >= 500;
}

// How long retry-after asks for, if it's a whole number of seconds.
std::optional<milliseconds> RetryAfter(const http::Response &r) {
  const auto it = r.headers.find("retry-after");
  if (it == r.headers.end()) {
    return std::nullopt;
  }
  long seconds = 0;
  const std::string &v = it->second;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), seconds);
  if (ec != std::errc() || end != v.data() + v.size() || seconds < 0) {
    return std::nullopt;
  }
  return std::min(milliseconds(seconds * 1000), kMaxWait);
}

bool Sleep(milliseconds delay, const std::function<bool()> &keep_going) {
  constexpr milliseconds kStep{100};
  for (milliseconds slept{0}; slept < delay; slept += kStep) {
    if (!keep_going()) {
      return false;
    }
    std::this_thread::sleep_for(std::min(kStep, delay - slept));
  }
  return keep_going();
}

}  // namespace

const char *ToString(ClaudeFailure f) {
  switch (f) {  // GCOVR_EXCL_BR_LINE: gcc's branch for values that aren't an enumerator
    case ClaudeFailure::kRefused:
      return "refused";
    case ClaudeFailure::kTruncated:
      return "truncated";
    case ClaudeFailure::kBadRequest:
      return "bad_request";
    case ClaudeFailure::kAuth:
      return "auth";
    case ClaudeFailure::kUnavailable:
      return "unavailable";
    case ClaudeFailure::kBadResponse:
      return "bad_response";
    case ClaudeFailure::kCancelled:
      return "cancelled";
  }
  return "?";  // GCOVR_EXCL_LINE: every value is handled above
}

std::expected<ClaudeReply, ClaudeError> ParseClaudeReply(std::string_view body) {
  const json r = json::parse(body, nullptr, false);
  if (!r.is_object()) {
    return Failed(ClaudeFailure::kBadResponse, "the answer isn't JSON");
  }
  const std::string *stop = Text(r, "stop_reason");
  if (stop != nullptr && *stop == "refusal") {
    std::string why = "Claude declined to draft this";
    const auto d = r.find("stop_details");
    if (d != r.end() && d->is_object()) {
      if (const std::string *category = Text(*d, "category"); category != nullptr) {
        why += " (" + *category + ")";
      }
    }
    return Failed(ClaudeFailure::kRefused, why);
  }
  if (stop != nullptr && *stop == "max_tokens") {
    return Failed(ClaudeFailure::kTruncated, "the answer was cut off at max_tokens");
  }
  ClaudeReply reply;
  const auto content = r.find("content");
  if (content == r.end() || !content->is_array()) {
    return Failed(ClaudeFailure::kBadResponse, "the answer has no content");
  }
  // Thinking blocks (empty with this model) and fallback markers are skipped.
  for (const json &block : *content) {
    const std::string *type = block.is_object() ? Text(block, "type") : nullptr;
    if (type == nullptr) {
      continue;
    }
    if (*type == "fallback") {
      reply.fell_back = true;
    } else if (*type == "text") {
      if (const std::string *t = Text(block, "text"); t != nullptr) {
        reply.text += *t;
      }
    }
  }
  if (reply.text.empty()) {
    return Failed(ClaudeFailure::kBadResponse, "the answer has no text");
  }
  if (const std::string *m = Text(r, "model"); m != nullptr) {
    reply.model = *m;
  }
  const auto usage = r.find("usage");
  if (usage != r.end() && usage->is_object()) {
    reply.input_tokens = Count(*usage, "input_tokens");
    reply.output_tokens = Count(*usage, "output_tokens");
    const auto iterations = usage->find("iterations");
    if (iterations != usage->end() && iterations->is_array()) {
      for (const json &i : *iterations) {
        const std::string *type = i.is_object() ? Text(i, "type") : nullptr;
        reply.fell_back = reply.fell_back || (type != nullptr && *type == "fallback_message");
      }
    }
  }
  return reply;
}

std::expected<ClaudeReply, ClaudeError> SendToClaude(const ClaudeClient &client,
                                                     const std::string &body,
                                                     const std::function<bool()> &keep_going) {
  http::Request request{.url = client.base_url + "/v1/messages",
                        .headers = {{"x-api-key", client.api_key},
                                    {"anthropic-version", std::string(kClaudeApiVersion)},
                                    {"content-type", "application/json"}},
                        .body = body};  // GCOVR_EXCL_BR_LINE: gcc's exception cleanup
  // The beta header goes with the field it enables: sent without it, a
  // "fallbacks" field is refused, and the header alone is harmless but noise.
  const json parsed = json::parse(body, nullptr, false);
  if (parsed.is_object() && parsed.contains("fallbacks")) {
    request.headers.emplace_back("anthropic-beta", std::string(kClaudeFallbackBeta));
  }
  const std::function<bool()> go = keep_going ? keep_going : [] { return true; };
  const auto wait = [&](milliseconds delay) {
    return client.wait ? client.wait(delay) : Sleep(delay, go);
  };
  for (int attempt = 0;; ++attempt) {
    if (keep_going && !keep_going()) {
      return Failed(ClaudeFailure::kCancelled, "stopped");
    }
    const milliseconds backoff = std::min(milliseconds(1000) * (1 << attempt), kMaxWait);
    milliseconds delay = backoff;
    std::string why;
    const auto response = client.http->Post(request, keep_going);
    if (!response) {
      if (response.error() == http::Error::kCancelled) {
        return Failed(ClaudeFailure::kCancelled, "stopped");
      }
      why = std::string("can't reach the Claude API: ") + http::ToString(response.error());
    } else if (response->status >= 200 && response->status < 300) {
      return ParseClaudeReply(response->body);
    } else if (Retryable(response->status)) {
      why = ApiMessage(response->body, response->status);
      delay = RetryAfter(*response).value_or(backoff);
    } else {
      const long s = response->status;
      const bool auth = s == 401 || s == 402 || s == 403;
      return Failed(auth ? ClaudeFailure::kAuth : ClaudeFailure::kBadRequest,
                    ApiMessage(response->body, s));
    }
    if (attempt >= client.max_retries) {
      return Failed(ClaudeFailure::kUnavailable, why);
    }
    if (!wait(delay)) {
      return Failed(ClaudeFailure::kCancelled, "stopped");
    }
  }
}

}  // namespace micnote::planner
