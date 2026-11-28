// The real HTTPS client (stage 3 §4.6): libcurl, with the system's
// certificates, in the optional micnote_http target (-DMICNOTE_HTTPS=ON).
// Only http and https; no redirects followed (the Claude API doesn't send
// any, and a redirect mustn't carry the API key elsewhere); certificates are
// always checked. Each request gets its own connection handle, so a client
// can be shared between threads.
#pragma once

#include "http/http.hpp"

namespace micnote::http {

class CurlClient final : public Client {
 public:
  CurlClient();
  std::expected<Response, Error> Post(const Request &request,
                                      const std::function<bool()> &keep_going) override;
};

}  // namespace micnote::http
