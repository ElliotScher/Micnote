#include "planner/redact.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace micnote::planner {

namespace {

using nlohmann::json;

bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Upper(char c) { return c >= 'A' && c <= 'Z'; }
bool Lower(char c) { return c >= 'a' && c <= 'z'; }
bool Alnum(char c) { return Digit(c) || Upper(c) || Lower(c); }
bool Hex(char c) { return Digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
// Part of a word, for whole-word matches: UTF-8 bytes beyond ASCII count, so
// "Café" isn't matched inside "Cafés".
bool WordChar(char c) { return Alnum(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
char Lowered(char c) { return Upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }

// Characters around a token that aren't part of it: "(see ghp_x...)."
bool Wrapper(char c) {
  constexpr std::string_view kWrappers = "\"'(),;:!?<>.`";
  return kWrappers.find(c) != std::string_view::npos;
}

// "2026-11-24" at s[i]: dates aren't phone numbers.
bool DateAt(std::string_view s, std::size_t i) {
  constexpr std::string_view kShape = "dddd-dd-dd";
  if (s.size() - i < kShape.size()) {
    return false;
  }
  for (std::size_t k = 0; k < kShape.size(); ++k) {
    if (kShape[k] == 'd' ? !Digit(s[i + k]) : s[i + k] != '-') {
      return false;
    }
  }
  return true;
}

bool PhoneSeparator(char c) { return c == ' ' || c == '-' || c == '.' || c == '(' || c == ')'; }

// The end of a phone number starting at s[i], or i if there's none there.
std::size_t PhoneEnd(std::string_view s, std::size_t i) {
  const bool plus = s[i] == '+';
  std::size_t j = plus ? i + 1 : i;
  int digits = 0;
  int groups = 0;
  bool other_separator = false;  // one besides "."
  bool separated = false;
  bool in_group = false;
  while (j < s.size()) {
    if (Digit(s[j])) {
      ++digits;
      groups += in_group ? 0 : 1;
      in_group = true;
      ++j;
      continue;
    }
    // Up to three separators, then a digit (not a date's).
    std::size_t k = j;
    while (k < s.size() && k - j < 3 && PhoneSeparator(s[k])) {
      other_separator = other_separator || s[k] != '.';
      ++k;
    }
    if (k == j || k == s.size() || !Digit(s[k]) || DateAt(s, k)) {
      break;
    }
    separated = separated || in_group;
    in_group = false;
    j = k;
  }
  // Whatever follows mustn't carry on the word: "12345678abc" isn't one.
  if (j < s.size() && WordChar(s[j])) {
    return i;
  }
  // "3.14159265" and "1.2.3" aren't, but 555.123.4567 is.
  const bool dotted_only = separated && !other_separator;
  const bool grouped = separated && (!dotted_only || groups >= 3);
  if (digits < 7 || digits > 15 || (!plus && !grouped && digits < 10)) {
    return i;
  }
  return j;
}

bool LocalChar(char c) {
  return Alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}
bool DomainChar(char c) { return Alnum(c) || c == '.' || c == '-'; }

bool ValidDomain(std::string_view d) {
  const std::size_t dot = d.rfind('.');
  if (dot == std::string_view::npos || d.front() == '.' || d.find("..") != std::string_view::npos) {
    return false;
  }
  const std::string_view tld = d.substr(dot + 1);
  return tld.size() >= 2 && std::ranges::all_of(tld, [](char c) { return Upper(c) || Lower(c); });
}

}  // namespace

bool LooksSecret(std::string_view token) {
  if (token == "-----BEGIN") {  // a PEM key's header
    return true;
  }
  constexpr std::array<std::string_view, 15> kPrefixes = {
      "ghp_",  "gho_",  "ghu_",  "ghs_", "ghr_", "github_pat_", "glpat-", "xoxb-",
      "xoxp-", "xoxa-", "xoxs-", "sk-",  "AKIA", "ASIA",        "AIza"};
  for (const std::string_view p : kPrefixes) {
    if (token.starts_with(p) && token.size() >= p.size() + 12) {
      return true;
    }
  }
  if (token.size() < 32) {
    return false;
  }
  if (std::ranges::all_of(token, Hex)) {
    return true;
  }
  bool upper = false;
  bool lower = false;
  bool digit = false;
  for (const char c : token) {
    if (!Alnum(c) && c != '+' && c != '/' && c != '=' && c != '_' && c != '-') {
      return false;
    }
    upper = upper || Upper(c);
    lower = lower || Lower(c);
    digit = digit || Digit(c);
  }
  return upper && lower && digit;
}

Redactor::Redactor(RedactOptions options) : options_(std::move(options)) {
  // Longest first, so "Acme Labs" goes before "Acme".
  const auto longest = [](const std::string &a, const std::string &b) {
    return a.size() > b.size();
  };
  std::ranges::stable_sort(options_.terms, longest);
  std::ranges::stable_sort(options_.names, longest);
}

std::string Redactor::Placeholder(const char *kind, std::string_view original) {
  if (const auto it = placeholders_.find(original); it != placeholders_.end()) {
    return it->second;
  }
  const int n = ++counts_[kind];
  std::string p = "[" + std::string(kind) + "_" + std::to_string(n) + "]";
  placeholders_.emplace(original, p);
  originals_.emplace(p, original);
  return p;
}

std::string Redactor::Secret() {
  ++dropped_;
  return "[SECRET]";
}

std::string Redactor::Redact(std::string_view text) {
  std::string s(text);
  std::ranges::replace(s, '[', '(');
  std::ranges::replace(s, ']', ')');

  // Tokens: secrets, and URLs.
  {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
      if (Space(s[i])) {
        out += s[i++];
        continue;
      }
      std::size_t end = i;
      while (end < s.size() && !Space(s[end])) {
        ++end;
      }
      std::size_t b = i;
      std::size_t e = end;
      while (b < e && Wrapper(s[b])) {
        ++b;
      }
      while (e > b && Wrapper(s[e - 1])) {
        --e;
      }
      const std::string_view core = std::string_view(s).substr(b, e - b);
      out.append(s, i, b - i);
      if (core.starts_with("http://") || core.starts_with("https://")) {
        const std::size_t scheme = core.find("//") + 2;
        std::string url(core.substr(0, scheme));
        std::string_view rest = core.substr(scheme);
        const std::size_t q = rest.find_first_of("?#");
        rest = rest.substr(0, q);
        std::size_t from = 0;
        for (std::size_t at = 0; at <= rest.size(); ++at) {
          if (at == rest.size() || rest[at] == '/') {
            const std::string_view part = rest.substr(from, at - from);
            if (LooksSecret(part)) {
              url += Secret();
            } else {
              url += part;
            }
            url += at == rest.size() ? "" : "/";
            from = at + 1;
          }
        }
        if (q != std::string_view::npos) {
          ++dropped_;
          url += "?[REDACTED]";
        }
        // user:password@host
        const std::size_t host = scheme;
        const std::size_t slash = url.find('/', host);
        const std::size_t at = url.find('@', host);
        if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
          url.replace(host, at - host, Secret());
        }
        out += url;
      } else {
        if (LooksSecret(core)) {
          out += Secret();
        } else {
          out += core;
        }
      }
      out.append(s, e, end - e);
      i = end;
    }
    s = std::move(out);
  }

  // Email addresses.
  {
    std::string out;
    std::size_t done = 0;
    std::size_t from = 0;
    std::size_t p = 0;
    while ((p = s.find('@', from)) != std::string::npos) {
      std::size_t l = p;
      while (l > done && LocalChar(s[l - 1])) {
        --l;
      }
      std::size_t r = p + 1;
      while (r < s.size() && DomainChar(s[r])) {
        ++r;
      }
      while (r > p + 1 && (s[r - 1] == '.' || s[r - 1] == '-')) {
        --r;
      }
      if (l < p && r > p + 1 && ValidDomain(std::string_view(s).substr(p + 1, r - p - 1))) {
        out.append(s, done, l - done);
        out += Placeholder("EMAIL", std::string_view(s).substr(l, r - l));
        done = r;
        from = r;
      } else {
        from = p + 1;
      }
    }
    out.append(s, done);
    s = std::move(out);
  }

  // Phone numbers.
  {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      const bool starts = Digit(c) || c == '+' || c == '(';
      if (!starts || (i > 0 && (WordChar(s[i - 1]) || s[i - 1] == '+' || s[i - 1] == '-'))) {
        out += s[i++];
        continue;
      }
      if (DateAt(s, i)) {
        out.append(s, i, 10);
        i += 10;
        continue;
      }
      const std::size_t first = c == '(' ? i + 1 : i;
      const std::size_t end = first < s.size() ? PhoneEnd(s, first) : first;
      if (end == first) {
        out += s[i++];
        continue;
      }
      out += Placeholder("PHONE", std::string_view(s).substr(i, end - i));
      i = end;
    }
    s = std::move(out);
  }

  // The user's terms, then names: whole words, any case.
  const auto words = [&](const std::vector<std::string> &list, const char *kind) {
    for (const std::string &term : list) {
      if (term.empty()) {
        continue;
      }
      std::string lowered_s = s;
      std::ranges::transform(lowered_s, lowered_s.begin(), Lowered);
      std::string lowered_t = term;
      std::ranges::transform(lowered_t, lowered_t.begin(), Lowered);
      std::string out;
      std::size_t done = 0;
      std::size_t p = 0;
      while ((p = lowered_s.find(lowered_t, p)) != std::string::npos) {
        const std::size_t e = p + lowered_t.size();
        const bool whole = (p == 0 || !WordChar(s[p - 1])) && (e == s.size() || !WordChar(s[e]));
        if (whole) {
          out.append(s, done, p - done);
          out += Placeholder(kind, term);
          done = e;
        }
        p = whole ? e : p + 1;
      }
      out.append(s, done);
      s = std::move(out);
    }
  };
  words(options_.terms, "TERM");
  words(options_.names, "PERSON");
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

json Redactor::RedactJson(const json &value) {
  if (value.is_string()) {
    return Redact(value.get<std::string>());
  }
  json out = value;
  if (value.is_structured()) {
    for (json &v : out) {
      v = RedactJson(v);
    }
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

std::string Redactor::Restore(std::string_view text) const {
  std::string out;
  std::size_t done = 0;
  std::size_t p = 0;
  while ((p = text.find('[', p)) != std::string_view::npos) {
    const std::size_t close = text.find(']', p);
    if (close == std::string_view::npos) {
      break;
    }
    const auto it = originals_.find(text.substr(p, close - p + 1));
    if (it == originals_.end()) {
      ++p;
      continue;
    }
    out.append(text.substr(done, p - done));
    out += it->second;
    done = close + 1;
    p = done;
  }
  out.append(text.substr(done));
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

json Redactor::RestoreJson(const json &value) const {
  if (value.is_string()) {
    return Restore(value.get<std::string>());
  }
  json out = value;
  if (value.is_structured()) {
    for (json &v : out) {
      v = RestoreJson(v);
    }
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup

}  // namespace micnote::planner
