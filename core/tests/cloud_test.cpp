// The Claude backend and the cloud request, against a scripted HTTP client:
// no network, and never the real API.
#include "planner/cloud.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "crypto/psa.hpp"
#include "planner/schema_registry.hpp"

namespace {

namespace http = micnote::http;
using micnote::planner::ClaudeClient;
using micnote::planner::ClaudeFailure;
using micnote::planner::CloudSettings;
using micnote::planner::Item;
using micnote::planner::ParseClaudeReply;
using micnote::planner::SchemaRegistry;
using micnote::planner::SendToClaude;
using nlohmann::json;
using std::chrono::milliseconds;

class FakeHttp final : public http::Client {
 public:
  std::deque<std::expected<http::Response, http::Error>> replies;
  std::vector<http::Request> sent;

  std::expected<http::Response, http::Error> Post(
      const http::Request &request, const std::function<bool()> & /*keep_going*/) override {
    sent.push_back(request);
    auto r = replies.front();
    replies.pop_front();
    return r;
  }

  void Reply(long status, std::string body, std::map<std::string, std::string> headers = {}) {
    replies.emplace_back(
        http::Response{.status = status, .headers = std::move(headers), .body = std::move(body)});
  }
};

std::string Answer(const std::string &text, const std::string &extra = {}) {
  return json{{"model", "claude-opus-5-5"},
              {"stop_reason", "end_turn"},
              {"content",
               {{{"type", "thinking"}, {"thinking", ""}}, {{"type", "text"}, {"text", text}}}},
              {"usage", {{"input_tokens", 1200}, {"output_tokens", 80}}}}
             .dump() +
         extra;
}

struct Waits {
  std::vector<milliseconds> asked;
  bool ok = true;
};

ClaudeClient Client(FakeHttp &http, Waits &waits) {
  return ClaudeClient{.http = &http,
                      .api_key = "test-key",
                      .base_url = "http://mock",
                      .max_retries = 2,
                      .wait = [&waits](milliseconds d) {
                        waits.asked.push_back(d);
                        return waits.ok;
                      }};
}

std::string Header(const http::Request &r, const std::string &name) {
  for (const auto &[k, v] : r.headers) {
    if (k == name) {
      return v;
    }
  }
  return "(none)";
}

TEST(ClaudeReply, Parses) {
  const auto r = ParseClaudeReply(Answer(R"({"a":1})"));
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, R"({"a":1})");
  EXPECT_EQ(r->model, "claude-opus-5-5");
  EXPECT_FALSE(r->fell_back);
  EXPECT_EQ(r->input_tokens, 1200);
  EXPECT_EQ(r->output_tokens, 80);

  // A fallback took over: marked in the content, or only in usage.
  const auto f = ParseClaudeReply(R"({"stop_reason":"end_turn","content":[7,{"x":1},
      {"type":"fallback"},{"type":"text"},{"type":"text","text":"{}"}],
      "usage":{"input_tokens":"many","iterations":[3,{"x":1},{"type":"message"}]}})");
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->fell_back);
  EXPECT_EQ(f->model, "");
  EXPECT_EQ(f->input_tokens, 0);
  const auto u = ParseClaudeReply(R"({"content":[{"type":"text","text":"{}"}],
      "usage":{"iterations":[5,{"type":"message"},{"type":"fallback_message"}]}})");
  ASSERT_TRUE(u);
  EXPECT_TRUE(u->fell_back);
  const auto n = ParseClaudeReply(R"({"content":[{"type":"text","text":"{}"}],"usage":[]})");
  ASSERT_TRUE(n);
  EXPECT_EQ(n->output_tokens, 0);
  EXPECT_TRUE(
      ParseClaudeReply(R"({"content":[{"type":"text","text":"{}"}],"usage":{"iterations":{}}})"));
  EXPECT_TRUE(ParseClaudeReply(R"({"content":[{"type":"text","text":"{}"}]})"));  // no usage
}

TEST(ClaudeReply, Failures) {
  const auto kind = [](std::string_view body) { return ParseClaudeReply(body).error().kind; };
  EXPECT_EQ(kind("not json"), ClaudeFailure::kBadResponse);
  EXPECT_EQ(kind("[]"), ClaudeFailure::kBadResponse);
  EXPECT_EQ(kind(R"({"stop_reason":"max_tokens","content":[]})"), ClaudeFailure::kTruncated);
  EXPECT_EQ(kind(R"({"stop_reason":"end_turn"})"), ClaudeFailure::kBadResponse);
  EXPECT_EQ(kind(R"({"content":"text"})"), ClaudeFailure::kBadResponse);
  EXPECT_EQ(kind(R"({"content":[{"type":"thinking","thinking":""}]})"),
            ClaudeFailure::kBadResponse);

  const auto refused = ParseClaudeReply(
      R"({"stop_reason":"refusal","stop_details":{"type":"refusal","category":"cyber"},"content":[]})");
  EXPECT_EQ(refused.error().kind, ClaudeFailure::kRefused);
  EXPECT_EQ(refused.error().message, "Claude declined to draft this (cyber)");
  EXPECT_EQ(ParseClaudeReply(R"({"stop_reason":"refusal","stop_details":{"category":null}})")
                .error()
                .message,
            "Claude declined to draft this");
  EXPECT_EQ(ParseClaudeReply(R"({"stop_reason":"refusal","stop_details":null})").error().message,
            "Claude declined to draft this");
  EXPECT_EQ(ParseClaudeReply(R"({"stop_reason":"refusal"})").error().kind, ClaudeFailure::kRefused);
}

TEST(ClaudeSend, HeadersAndBody) {
  FakeHttp http;
  Waits waits;
  http.Reply(200, Answer("{}"));
  const std::string body = R"({"fallbacks":"default","model":"claude-opus-5-5"})";
  ASSERT_TRUE(SendToClaude(Client(http, waits), body));
  ASSERT_EQ(http.sent.size(), 1U);
  const http::Request &r = http.sent[0];
  EXPECT_EQ(r.url, "http://mock/v1/messages");
  EXPECT_EQ(r.body, body);
  EXPECT_EQ(Header(r, "x-api-key"), "test-key");
  EXPECT_EQ(Header(r, "anthropic-version"), "2026-10-21");
  EXPECT_EQ(Header(r, "content-type"), "application/json");
  EXPECT_EQ(Header(r, "anthropic-beta"), "server-side-fallback-2026-07-01");

  // No fallbacks field, no beta header; a 2xx other than 200 is read too.
  http.Reply(201, Answer("{}"));
  ASSERT_TRUE(SendToClaude(Client(http, waits), R"({"model":"claude-haiku-5-5"})"));
  EXPECT_EQ(Header(http.sent[1], "anthropic-beta"), "(none)");
  http.Reply(200, Answer("{}"));
  ASSERT_TRUE(SendToClaude(Client(http, waits), "not json"));
  EXPECT_EQ(Header(http.sent[2], "anthropic-beta"), "(none)");
  EXPECT_TRUE(waits.asked.empty());
}

TEST(ClaudeSend, ErrorsThatArentRetried) {
  FakeHttp http;
  Waits waits;
  const auto failure = [&](long status, std::string body) {
    http.Reply(status, std::move(body));
    return SendToClaude(Client(http, waits), "{}").error();
  };
  const auto bad = failure(
      400, R"({"type":"error","error":{"type":"invalid_request_error","message":"bad schema"}})");
  EXPECT_EQ(bad.kind, ClaudeFailure::kBadRequest);
  EXPECT_EQ(bad.message, "bad schema");
  EXPECT_EQ(failure(404, "<html>").message, "HTTP 404");
  EXPECT_EQ(failure(413, R"({"error":"big"})").message, "HTTP 413");
  EXPECT_EQ(failure(400, R"({"error":{"type":"x"}})").message, "HTTP 400");
  EXPECT_EQ(failure(400, R"({"other":1})").message, "HTTP 400");
  EXPECT_EQ(failure(302, "").message, "HTTP 302");
  EXPECT_EQ(failure(100, "").message, "HTTP 100");
  EXPECT_EQ(failure(401, "{}").kind, ClaudeFailure::kAuth);
  EXPECT_EQ(failure(402, "{}").kind, ClaudeFailure::kAuth);
  EXPECT_EQ(failure(403, "{}").kind, ClaudeFailure::kAuth);
  EXPECT_EQ(http.sent.size(), 10U);  // one try each
  EXPECT_TRUE(waits.asked.empty());
}

TEST(ClaudeSend, RetriesThenGivesUp) {
  FakeHttp http;
  Waits waits;
  http.Reply(529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
  http.Reply(500, "{}");
  http.Reply(503, "{}");
  const auto r = SendToClaude(Client(http, waits), "{}");
  EXPECT_EQ(r.error().kind, ClaudeFailure::kUnavailable);
  EXPECT_EQ(r.error().message, "HTTP 503");
  EXPECT_EQ(http.sent.size(), 3U);  // the try and two retries
  EXPECT_EQ(waits.asked, (std::vector<milliseconds>{milliseconds(1000), milliseconds(2000)}));
}

TEST(ClaudeSend, RetryAfterAndNetwork) {
  FakeHttp http;
  Waits waits;
  http.Reply(429, "{}", {{"retry-after", "3"}});
  http.replies.emplace_back(std::unexpected(http::Error::kConnect));
  http.Reply(200, Answer("{}"));
  ASSERT_TRUE(SendToClaude(Client(http, waits), "{}"));
  EXPECT_EQ(waits.asked, (std::vector<milliseconds>{milliseconds(3000), milliseconds(2000)}));

  // retry-after that isn't a whole number of seconds is ignored; a long one
  // is capped at a minute.
  for (const char *odd : {"soon", "-1", "5x", ""}) {
    waits.asked.clear();
    http.Reply(408, "{}", {{"retry-after", odd}});
    http.Reply(200, Answer("{}"));
    ASSERT_TRUE(SendToClaude(Client(http, waits), "{}"));
    EXPECT_EQ(waits.asked, (std::vector<milliseconds>{milliseconds(1000)})) << odd;
  }
  waits.asked.clear();
  http.Reply(409, "{}", {{"retry-after", "3600"}});
  http.Reply(200, Answer("{}"));
  ASSERT_TRUE(SendToClaude(Client(http, waits), "{}"));
  EXPECT_EQ(waits.asked, (std::vector<milliseconds>{milliseconds(60000)}));

  http.replies.emplace_back(std::unexpected(http::Error::kTls));
  http.replies.emplace_back(std::unexpected(http::Error::kTimeout));
  http.replies.emplace_back(std::unexpected(http::Error::kOther));
  EXPECT_EQ(SendToClaude(Client(http, waits), "{}").error().message,
            "can't reach the Claude API: failed");
}

TEST(ClaudeSend, Cancelled) {
  FakeHttp http;
  Waits waits;
  EXPECT_EQ(SendToClaude(Client(http, waits), "{}", [] { return false; }).error().kind,
            ClaudeFailure::kCancelled);
  EXPECT_TRUE(http.sent.empty());
  http.replies.emplace_back(std::unexpected(http::Error::kCancelled));
  EXPECT_EQ(SendToClaude(Client(http, waits), "{}").error().kind, ClaudeFailure::kCancelled);
  waits.ok = false;
  http.Reply(500, "{}");
  EXPECT_EQ(SendToClaude(Client(http, waits), "{}").error().kind, ClaudeFailure::kCancelled);
}

TEST(ClaudeSend, SleepsWithoutAWaiter) {
  FakeHttp http;
  ClaudeClient c{
      .http = &http, .api_key = "k", .base_url = "http://mock", .max_retries = 2, .wait = {}};
  // retry-after 0: no time at all.
  http.Reply(500, "{}", {{"retry-after", "0"}});
  http.Reply(200, Answer("{}"));
  EXPECT_TRUE(SendToClaude(c, "{}", [] { return true; }));
  http.Reply(500, "{}", {{"retry-after", "0"}});
  http.Reply(200, Answer("{}"));
  EXPECT_TRUE(SendToClaude(c, "{}"));
  // Stopped while waiting.
  int calls = 0;
  http.Reply(500, "{}");
  EXPECT_EQ(SendToClaude(c, "{}", [&] { return ++calls < 2; }).error().kind,
            ClaudeFailure::kCancelled);
  // Otherwise it sleeps the whole second.
  http.Reply(500, "{}");
  http.Reply(200, Answer("{}"));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(SendToClaude(c, "{}", [] { return true; }));
  EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(1000));
}

TEST(ClaudeSend, Names) {
  for (const auto f :
       {ClaudeFailure::kRefused, ClaudeFailure::kTruncated, ClaudeFailure::kBadRequest,
        ClaudeFailure::kAuth, ClaudeFailure::kUnavailable, ClaudeFailure::kBadResponse,
        ClaudeFailure::kCancelled}) {
    EXPECT_STRNE(micnote::planner::ToString(f), "?");
  }
  for (const auto e : {http::Error::kConnect, http::Error::kTls, http::Error::kTimeout,
                       http::Error::kCancelled, http::Error::kOther}) {
    EXPECT_STRNE(http::ToString(e), "?");
  }
}

// ---- the request ------------------------------------------------------------------

struct Fixture {
  SchemaRegistry registry = SchemaRegistry::Builtin().value();
  Item item;
  json context = {{"contacts", {{{"name", "Jeff"}, {"emails", {"jeff@example.com"}}}}}};
  micnote::planner::PlannerOptions options;

  Fixture() {
    item.kind = "todo";
    item.text = "Email Jeff";
    item.said = "email jeff@example.com about moving the budget review to Thursday";
    item.intent = registry.Find("email", "send_email");
    options.today = "2026-11-24";
  }
};

TEST(ClaudeSchema, Converts) {
  const json in = {
      {"type", "object"},
      {"additionalProperties", false},
      {"required", {"to", "when"}},
      {"properties",
       {{"to", {{"type", "array"}, {"items", {{"type", "string"}, {"format", "email"}}}}},
        {"when", {{"type", "string"}, {"format", "date-time"}, {"x-nullable", true}}},
        {"n", {{"type", "integer"}, {"minimum", 1}, {"description", "d"}}}}}};
  EXPECT_EQ(micnote::planner::ClaudeSchema(in),
            (json{{"type", "object"},
                  {"additionalProperties", false},
                  {"required", {"to", "when"}},
                  {"properties",
                   {{"to", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"when", {{"anyOf", {{{"type", "string"}}, {{"type", "null"}}}}}},
                    {"n", {{"type", "integer"}, {"description", "d"}}}}}}));
}

TEST(CloudRequest, IsRedactedAndHashed) {
  Fixture f;
  CloudSettings s;
  s.redact.terms = {"budget review"};
  const auto r = micnote::planner::BuildCloudRequest(f.item, f.context, f.options, s);
  const json body = json::parse(r.body);
  EXPECT_EQ(body["model"], "claude-opus-5-5");
  EXPECT_EQ(body["max_tokens"], 16000);
  EXPECT_EQ(body["fallbacks"], "default");
  EXPECT_EQ(body["output_config"]["effort"], "medium");
  EXPECT_EQ(body["output_config"]["format"]["type"], "json_schema");
  EXPECT_EQ(
      body["output_config"]["format"]["schema"]["properties"]["params"]["properties"]["to"],
      (json{{"type", "array"}, {"description", "Recipients."}, {"items", {{"type", "string"}}}}));
  EXPECT_FALSE(body.contains("temperature"));  // refused by API version 2026-10-21
  EXPECT_FALSE(body.contains("thinking"));     // always on with this model
  const std::string user = body["messages"][0]["content"];
  EXPECT_EQ(body["messages"][0]["role"], "user");
  EXPECT_NE(user.find("email [EMAIL_1] about moving the [TERM_1] to Thursday"), std::string::npos)
      << user;
  EXPECT_EQ(r.body.find("jeff@example.com"), std::string::npos);
  EXPECT_EQ(r.body.find("budget review"), std::string::npos);
  EXPECT_NE(body["system"].get<std::string>().find("2026-11-24"), std::string::npos);

  // The hash is the body's SHA-256, and the same inputs give the same body.
  const auto digest = micnote::crypto::Sha256(std::as_bytes(std::span(r.body)));
  std::string hex;
  for (const std::byte b : digest) {
    hex += std::format("{:02x}", std::to_integer<unsigned>(b));
  }
  EXPECT_EQ(r.hash, hex);
  EXPECT_EQ(micnote::planner::BuildCloudRequest(f.item, f.context, f.options, s).hash, r.hash);
  // Anything different, a different hash.
  f.context["contacts"][0]["emails"].push_back("jeff@work.example");
  EXPECT_NE(micnote::planner::BuildCloudRequest(f.item, f.context, f.options, s).hash, r.hash);

  // Haiku has no server-side fallback: no field for it.
  s.model = "claude-haiku-5-5";
  const json haiku =
      json::parse(micnote::planner::BuildCloudRequest(f.item, f.context, f.options, s).body);
  EXPECT_FALSE(haiku.contains("fallbacks"));
}

TEST(CloudRequest, MoreTokens) {
  EXPECT_EQ(json::parse(micnote::planner::WithMoreTokens(R"({"max_tokens":16000,"x":1})")),
            (json{{"max_tokens", 32000}, {"x", 1}}));
  EXPECT_EQ(json::parse(micnote::planner::WithMoreTokens(R"({"max_tokens":40000})"))["max_tokens"],
            64000);
}

const char *kDraft =
    R"({"params":{"to":["[EMAIL_1]","[EMAIL_7]"],"subject":"Budget review","body_markdown":"Hi, can we move it to Thursday? [EMAIL_1]"},"rationale":"asked to email [EMAIL_1]","confidence":"high"})";

TEST(CloudDraft, RestoresAndGrounds) {
  Fixture f;
  const auto r = micnote::planner::BuildCloudRequest(f.item, f.context, f.options, {});
  const json d =
      micnote::planner::FinishCloudDraft(r, f.item, f.context, {}, kDraft).value_or(json());
  // [EMAIL_7] was never sent: no original, and not an address anyone said.
  EXPECT_EQ(d["params"]["to"], json({"jeff@example.com"}));
  EXPECT_EQ(d["params"]["body_markdown"], "Hi, can we move it to Thursday? jeff@example.com");
  EXPECT_EQ(d["rationale"], "asked to email jeff@example.com");
  EXPECT_EQ(d["connector"], "email");
  EXPECT_FALSE(micnote::planner::FinishCloudDraft(r, f.item, f.context, {}, "nonsense"));
}

TEST(CloudDraft, SendsAndRetriesACutOffAnswer) {
  Fixture f;
  FakeHttp http;
  Waits waits;
  const auto r = micnote::planner::BuildCloudRequest(f.item, f.context, f.options, {});
  http.Reply(200, R"({"stop_reason":"max_tokens","content":[]})");
  http.Reply(200, Answer(kDraft));
  const auto d = micnote::planner::CloudDraft(Client(http, waits), r, f.item, f.context);
  ASSERT_TRUE(d) << d.error().message;
  EXPECT_EQ(d->draft["params"]["to"], json({"jeff@example.com"}));
  EXPECT_EQ(d->reply.output_tokens, 80);
  ASSERT_EQ(http.sent.size(), 2U);
  EXPECT_EQ(http.sent[0].body, r.body);
  EXPECT_EQ(json::parse(http.sent[1].body)["max_tokens"], 32000);

  http.Reply(200, R"({"stop_reason":"refusal","content":[]})");
  EXPECT_EQ(micnote::planner::CloudDraft(Client(http, waits), r, f.item, f.context).error().kind,
            ClaudeFailure::kRefused);
  http.Reply(200, Answer("[1,2]"));
  EXPECT_EQ(micnote::planner::CloudDraft(Client(http, waits), r, f.item, f.context).error().kind,
            ClaudeFailure::kBadResponse);
}

}  // namespace
