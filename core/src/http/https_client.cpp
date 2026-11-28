#include "http/https_client.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string_view>

#include <curl/curl.h>

namespace micnote::http {

namespace {

struct Transfer {
  std::string body;
  std::map<std::string, std::string> headers;
  const std::function<bool()> *keep_going = nullptr;
};

std::size_t OnBody(char *data, std::size_t size, std::size_t n, void *user) {
  static_cast<Transfer *>(user)->body.append(data, size * n);
  return size * n;
}

std::size_t OnHeader(char *data, std::size_t size, std::size_t n, void *user) {
  auto &t = *static_cast<Transfer *>(user);
  const std::string_view line(data, size * n);
  if (line.starts_with("HTTP/")) {
    t.headers.clear();  // a new response (after "100 Continue", say)
    return size * n;
  }
  const std::size_t colon = line.find(':');
  if (colon != std::string_view::npos) {
    std::string name(line.substr(0, colon));
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
      value.remove_suffix(1);
    }
    t.headers[name] = value;
  }
  return size * n;
}

int OnProgress(void *user, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
               curl_off_t /*ulnow*/) {
  const auto &t = *static_cast<Transfer *>(user);
  return (*t.keep_going && !(*t.keep_going)()) ? 1 : 0;
}

Error FromCurl(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
      return Error::kConnect;
    case CURLE_OPERATION_TIMEDOUT:
      return Error::kTimeout;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
      return Error::kTls;
    case CURLE_ABORTED_BY_CALLBACK:
      return Error::kCancelled;
    default:
      return Error::kOther;
  }
}

struct EasyDeleter {
  void operator()(CURL *c) const { curl_easy_cleanup(c); }
};
struct ListDeleter {
  void operator()(curl_slist *l) const { curl_slist_free_all(l); }
};

}  // namespace

CurlClient::CurlClient() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::expected<Response, Error> CurlClient::Post(const Request &request,
                                                const std::function<bool()> &keep_going) {
  const std::unique_ptr<CURL, EasyDeleter> curl(curl_easy_init());
  if (!curl) {
    return std::unexpected(Error::kOther);
  }
  curl_slist *list = nullptr;
  const auto add = [&](const std::string &line) {
    curl_slist *more = curl_slist_append(list, line.c_str());
    if (more == nullptr) {
      return false;
    }
    list = more;
    return true;
  };
  bool added = true;
  for (const auto &[name, value] : request.headers) {
    std::string line = name;
    line += ": ";
    line += value;
    added = added && add(line);
  }
  added = added && add("Expect:");  // no "100 Continue" round trip before a large body
  const std::unique_ptr<curl_slist, ListDeleter> headers(list);
  if (!added) {
    return std::unexpected(Error::kOther);
  }

  Transfer t;
  t.keep_going = &keep_going;
  CURL *c = curl.get();
  curl_easy_setopt(c, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, request.body.data());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(request.timeout.count()));
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  // The operating system's certificates where curl can use them (Windows,
  // macOS); elsewhere curl's default store, which is the system's.
  curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &OnBody);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, &OnHeader);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &OnProgress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);

  const CURLcode code = curl_easy_perform(c);
  if (code != CURLE_OK) {
    return std::unexpected(FromCurl(code));
  }
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  return Response{.status = status, .headers = std::move(t.headers), .body = std::move(t.body)};
}

}  // namespace micnote::http
