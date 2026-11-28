// Redaction for the cloud path (stage 3 §4.5). Before an item goes to
// Claude, what identifies people or could unlock something is swapped for a
// placeholder: "[EMAIL_1]", "[PHONE_1]", the query of a URL, the user's own
// terms ("[TERM_1]"), optionally contacts' names ("[PERSON_1]"), and
// anything that looks like a secret ("[SECRET]"). The placeholder → original
// table stays here, in memory: Restore() puts the originals back into what
// Claude wrote. A secret is never kept, so it can't come back.
//
// These are heuristics, and the preview shows the user exactly what's left.
// - An email address: local@domain.tld.
// - A phone number: 7 to 15 digits in groups (spaces, "-", ".", "()"),
//   starting with "+" or written in groups, or 10 digits or more in a row.
//   A date (2026-11-24) is not one.
// - A URL (http:// or https://): its query and fragment become
//   "?[REDACTED]"; a path part that looks like a secret becomes [SECRET].
// - A secret: a known token prefix (ghp_, github_pat_, sk-, xoxb-, AKIA,
//   glpat-, ...), a private key header, 32 or more hex digits, or 32 or more
//   characters of base64 or URL-safe base64 with both letters and digits.
//   (So a 40-digit git commit hash counts as a secret too.)
// - Terms and names: whole words, ignoring case.
// Square brackets already in the text become parentheses, so nothing the
// user said can pass for a placeholder.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace micnote::planner {

struct RedactOptions {
  // From settings: client names, projects. Empty ones are ignored.
  std::vector<std::string> terms;
  // Names to hide (contacts' names). Off by default, since drafts need
  // names: the caller fills this only when the user turns it on.
  std::vector<std::string> names;
};

class Redactor {
 public:
  explicit Redactor(RedactOptions options);

  // The same original always gets the same placeholder.
  std::string Redact(std::string_view text);
  // Every string inside (object keys stay as they are).
  nlohmann::json RedactJson(const nlohmann::json &value);

  // The originals back in place of their placeholders; [SECRET] stays.
  [[nodiscard]] std::string Restore(std::string_view text) const;
  [[nodiscard]] nlohmann::json RestoreJson(const nlohmann::json &value) const;

  // How many things were replaced: each original with a placeholder once,
  // and each secret or URL query every time.
  [[nodiscard]] std::size_t Count() const { return originals_.size() + dropped_; }

 private:
  std::string Placeholder(const char *kind, std::string_view original);
  std::string Secret();

  RedactOptions options_;
  std::map<std::string, std::string, std::less<>> placeholders_;  // original → placeholder
  std::map<std::string, std::string, std::less<>> originals_;     // placeholder → original
  std::map<std::string, int, std::less<>> counts_;                // per kind
  std::size_t dropped_ = 0;  // secrets and queries: never restored
};

// Exposed for the tests.
bool LooksSecret(std::string_view token);

}  // namespace micnote::planner
