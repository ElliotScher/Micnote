// The libcurl client, and the Claude backend through it, against a mock
// server on 127.0.0.1: what goes over the wire, and how failures come back.
// Plain HTTP (the TLS case only checks that a non-TLS server is refused):
// nothing here leaves the machine or touches the real API.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "http/https_client.hpp"
#include "planner/llm/claude_backend.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

namespace http = micnote::http;
using namespace std::chrono_literals;

struct Received {
  std::string request_line;
  std::vector<std::string> headers;  // as sent: "Name: value"
  std::string body;
};

// One scripted answer per connection: raw bytes to send back, or (empty)
// hold the connection open without answering until the test ends.
class MockServer {
 public:
  explicit MockServer(std::deque<std::string> answers)
      : fd_(socket(AF_INET, SOCK_STREAM, 0)), answers_(std::move(answers)) {
    sockaddr_in a = Loopback(0);
    socklen_t len = sizeof a;
    if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 ||
        listen(fd_, 8) != 0 || getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len) != 0) {
      throw std::runtime_error("mock server: can't listen on 127.0.0.1");
    }
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }
  MockServer(const MockServer &) = delete;
  MockServer &operator=(const MockServer &) = delete;
  MockServer(MockServer &&) = delete;
  MockServer &operator=(MockServer &&) = delete;
  ~MockServer() {
    stop_ = true;
    // Wakes accept() (closing the socket doesn't, on every system).
    const int wake = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = Loopback(port_);
    // If it fails, accept() has failed already and the thread is on its way out.
    [[maybe_unused]] const int woke =
        wake >= 0 ? connect(wake, reinterpret_cast<sockaddr *>(&a), sizeof a) : -1;
    thread_.join();
    if (wake >= 0) {
      close(wake);
    }
    close(fd_);
  }

  [[nodiscard]] std::string Url(const char *scheme = "http") const {
    return std::string(scheme) + "://127.0.0.1:" + std::to_string(port_);
  }
  std::vector<Received> Seen() {
    const std::scoped_lock lock(mu_);
    return seen_;
  }

 private:
  static sockaddr_in Loopback(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    return a;
  }

  void Serve() {
    while (!stop_) {
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0 || stop_) {
        if (c >= 0) {
          close(c);
        }
        break;
      }
      std::string in;
      std::array<char, 65536> buf{};
      std::size_t header_end = std::string::npos;
      std::size_t length = 0;
      while (true) {
        if (header_end != std::string::npos && in.size() >= header_end + 4 + length) {
          break;
        }
        const ssize_t got = recv(c, buf.data(), buf.size(), 0);
        if (got <= 0) {
          break;
        }
        in.append(buf.data(), static_cast<std::size_t>(got));
        if (in.front() == '\x16') {  // a TLS handshake: answer in plain HTTP straight away
          break;
        }
        if (header_end == std::string::npos) {
          header_end = in.find("\r\n\r\n");
        }
        if (header_end != std::string::npos && length == 0) {
          const std::size_t cl = in.find("Content-Length: ");
          length = cl < header_end ? std::stoul(in.substr(cl + 16)) : 0;
        }
      }
      Received r;
      if (header_end != std::string::npos) {
        std::size_t at = 0;
        bool first = true;
        while (at < header_end) {
          const std::size_t eol = in.find("\r\n", at);
          const std::string line = in.substr(at, eol - at);
          (first ? r.request_line : r.headers.emplace_back()) = line;
          first = false;
          at = eol + 2;
        }
        r.body = in.substr(header_end + 4);
      }
      std::string answer;
      {
        const std::scoped_lock lock(mu_);
        seen_.push_back(std::move(r));
        if (!answers_.empty()) {
          answer = answers_.front();
          answers_.pop_front();
        }
      }
      if (answer.empty()) {
        held_.push_back(c);  // never answered; closed at the end
        continue;
      }
      send(c, answer.data(), answer.size(), 0);
      close(c);
    }
    for (const int c : held_) {
      close(c);
    }
  }

  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::deque<std::string> answers_;
  std::vector<Received> seen_;
  std::vector<int> held_;
  std::thread thread_;
};

std::string Reply(int status, const std::string &body, const std::string &extra_headers = {}) {
  return "HTTP/1.1 " + std::to_string(status) +
         " X\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers +
         "Connection: close\r\n\r\n" + body;
}

http::Request To(std::string url, std::chrono::seconds timeout = 600s) {
  return {.url = std::move(url), .headers = {}, .body = {}, .timeout = timeout};
}

bool Has(const Received &r, const std::string &header) {
  return std::ranges::find(r.headers, header) != r.headers.end();
}

TEST(CurlClient, PostsAndReads) {
  const std::string big(1 << 20, 'x');
  MockServer server({Reply(201, "made", "X-Thing:  A b \r\nRetry-After: 7\r\n"), Reply(200, "ok")});
  http::CurlClient client;
  http::Request req{.url = server.Url() + "/v1/messages",
                    .headers = {{"x-api-key", "k"}, {"content-type", "application/json"}},
                    .body = R"({"a":1})"};
  const auto r = client.Post(req, [] { return true; });
  ASSERT_TRUE(r) << http::ToString(r.error());
  EXPECT_EQ(r->status, 201);
  EXPECT_EQ(r->body, "made");
  EXPECT_EQ(r->headers.at("x-thing"), "A b");
  EXPECT_EQ(r->headers.at("retry-after"), "7");

  req.body = big;
  const auto r2 = client.Post(req, {});
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->body, "ok");

  const auto seen = server.Seen();
  ASSERT_EQ(seen.size(), 2U);
  EXPECT_EQ(seen[0].request_line, "POST /v1/messages HTTP/1.1");
  EXPECT_TRUE(Has(seen[0], "x-api-key: k"));
  EXPECT_TRUE(Has(seen[0], "content-type: application/json"));
  EXPECT_EQ(seen[0].body, R"({"a":1})");
  EXPECT_EQ(seen[1].body, big);
  for (const auto &h : seen[1].headers) {
    EXPECT_FALSE(h.starts_with("Expect")) << h;
  }
}

TEST(CurlClient, Failures) {
  http::CurlClient client;
  // A port nobody listens on.
  int port = 0;
  {
    MockServer gone({});
    port = std::stoi(gone.Url().substr(gone.Url().rfind(':') + 1));
  }
  EXPECT_EQ(client.Post(To("http://127.0.0.1:" + std::to_string(port)), {}).error(),
            http::Error::kConnect);
  EXPECT_EQ(client.Post(To("ftp://127.0.0.1/x"), {}).error(), http::Error::kOther);

  MockServer silent({});  // holds every connection without answering
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(client.Post(To(silent.Url(), 1s), {}).error(), http::Error::kTimeout);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
  const auto stop_at = std::chrono::steady_clock::now() + 300ms;
  EXPECT_EQ(
      client.Post(To(silent.Url()), [&] { return std::chrono::steady_clock::now() < stop_at; })
          .error(),
      http::Error::kCancelled);
  // TLS to a server that doesn't speak it.
  MockServer plain({"HTTP/1.1 200 OK\r\n\r\n"});
  EXPECT_EQ(client.Post(To(plain.Url("https")), {}).error(), http::Error::kTls);
}

TEST(CurlClient, ClaudeThroughTheWire) {
  const std::string answer =
      R"({"model":"claude-opus-5-5","stop_reason":"end_turn","content":[{"type":"text","text":"{\"ok\":true}"}],"usage":{"input_tokens":10,"output_tokens":3}})";
  MockServer server(
      {Reply(529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})",
             "Retry-After: 0\r\n"),
       Reply(200, answer)});
  http::CurlClient client;
  const micnote::planner::ClaudeClient claude{.http = &client,
                                              .api_key = "test-key",
                                              .base_url = server.Url(),
                                              .max_retries = 2,
                                              .wait = {}};
  const auto r = micnote::planner::SendToClaude(claude, R"({"fallbacks":"default"})");
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->text, R"({"ok":true})");
  const auto seen = server.Seen();
  ASSERT_EQ(seen.size(), 2U);
  for (const auto &s : seen) {
    EXPECT_EQ(s.request_line, "POST /v1/messages HTTP/1.1");
    EXPECT_TRUE(Has(s, "x-api-key: test-key"));
    EXPECT_TRUE(Has(s, "anthropic-version: 2026-10-21"));
    EXPECT_TRUE(Has(s, "anthropic-beta: server-side-fallback-2026-07-01"));
  }
}

}  // namespace
