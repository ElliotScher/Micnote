// How the core makes an HTTPS request (stage 3 §4.6): an interface, so the
// Claude backend can be tested against a scripted fake, and the libcurl
// implementation (https_client.hpp, the optional micnote_http target) stays
// out of the library's tests. Only POST, which is all the Claude API needs.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace micnote::http {

struct Request {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::seconds timeout{600};
};

struct Response {
  long status = 0;
  // Names in lower case.
  std::map<std::string, std::string> headers;
  std::string body;
};

enum class Error : std::uint8_t {
  kConnect,  // no route, refused, DNS
  kTls,      // the certificate or handshake failed
  kTimeout,
  kCancelled,  // keep_going said stop
  kOther,
};

class Client {
 public:
  Client() = default;
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  Client(Client &&) = delete;
  Client &operator=(Client &&) = delete;
  // GCOVR_EXCL_START: the deleting destructor only runs for an object whose
  // dynamic type is Client, which is abstract.
  virtual ~Client() = default;
  // GCOVR_EXCL_STOP

  // `keep_going`, called often while the request runs, cancels by returning false.
  virtual std::expected<Response, Error> Post(const Request &request,
                                              const std::function<bool()> &keep_going) = 0;
};

const char *ToString(Error e);

}  // namespace micnote::http
