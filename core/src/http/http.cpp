#include "http/http.hpp"

namespace micnote::http {

const char *ToString(Error e) {
  switch (e) {  // GCOVR_EXCL_BR_LINE: gcc's branch for values that aren't an enumerator
    case Error::kConnect:
      return "no connection";
    case Error::kTls:
      return "the secure connection failed";
    case Error::kTimeout:
      return "timed out";
    case Error::kCancelled:
      return "cancelled";
    case Error::kOther:
      return "failed";
  }
  return "?";  // GCOVR_EXCL_LINE: every value is handled above
}

}  // namespace micnote::http
