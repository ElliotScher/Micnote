#include "planner/action_validator.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>

namespace micnote::planner {

namespace {

using nlohmann::json;

// `n` digits at `at`, as a number; -1 if they aren't all digits.
int Digits(std::string_view s, std::size_t at, std::size_t n) {
  int v = 0;
  for (std::size_t i = at; i < at + n; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    v = (v * 10) + (s[i] - '0');
  }
  return v;
}

// Deliberately simple: one @, a dotted domain, nothing that could start
// another header or address.
bool Email(std::string_view s) {
  const auto at = s.find('@');
  if (at == 0 || at == std::string_view::npos || s.size() > 254) {
    return false;
  }
  const std::string_view domain = s.substr(at + 1);
  const bool plain = std::ranges::none_of(s, [](char c) {
    return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F ||
           std::string_view("\"(),:;<>[]\\").contains(c);
  });
  return plain && !domain.contains('@') && domain.contains('.') && !domain.starts_with('.') &&
         !domain.ends_with('.') && !domain.contains("..");
}

// Characters, not bytes (UTF-8 continuation bytes don't count).
std::size_t Length(std::string_view s) {
  return static_cast<std::size_t>(std::ranges::count_if(
      s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

// owner/name, as GitHub allows them: nothing that could change an API path.
bool Repo(std::string_view s) {
  const auto slash = s.find('/');
  if (slash == std::string_view::npos) {
    return false;
  }
  const auto part = [](std::string_view p) {
    return !p.empty() && p.size() <= 100 && p != "." && p != ".." &&
           std::ranges::all_of(p, [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
           });
  };
  return part(s.substr(0, slash)) && part(s.substr(slash + 1));
}

// GCOVR_EXCL_BR_START: here to the end, what gcov reports uncovered is gcc's
// exception cleanup for strings and json temporaries; every rule has a case
// in planner_schema_test.cpp, and line coverage still applies.
class Checker {
 public:
  Checker(const ActionType &action, const json &params, Stage stage, std::set<std::string> missing,
          const DiffLines *diff)
      : action_(&action),
        params_(&params),
        stage_(stage),
        missing_(std::move(missing)),
        diff_(diff) {}

  std::vector<FieldError> Run() {
    CheckValue(action_->params_schema, *params_, "");
    if (errors_.empty()) {  // the rules below read fields of the right type
      Semantics();
    }
    return std::move(errors_);
  }

 private:
  void Add(std::string path, std::string code, std::string message) {
    errors_.push_back(
        {.path = std::move(path), .code = std::move(code), .message = std::move(message)});
  }

  // A top-level field the planner said it couldn't fill.
  [[nodiscard]] bool Excused(const std::string &path) const {
    return stage_ == Stage::kDraft && path.starts_with('/') && missing_.contains(path.substr(1));
  }

  static bool Empty(const json &v) {
    return v.is_null() || (v.is_string() && v.get_ref<const std::string &>().empty()) ||
           (v.is_array() && v.empty());
  }

  void CheckValue(const json &schema, const json &v, const std::string &path) {
    const auto &type = schema["type"].get_ref<const std::string &>();
    if (type == "object") {
      CheckObject(schema, v, path);
    } else if (type == "array") {
      if (!v.is_array()) {
        Add(path, "type", "must be a list");
        return;
      }
      for (std::size_t i = 0; i < v.size(); ++i) {
        std::string at = path;
        at += '/';
        CheckValue(schema["items"], v[i], at += std::to_string(i));
      }
    } else if (type == "string") {
      CheckString(schema, v, path);
    } else if (type == "integer" ? !v.is_number_integer() : !v.is_boolean()) {
      Add(path, "type", type == "integer" ? "must be a whole number" : "must be true or false");
    }
  }

  void CheckObject(const json &schema, const json &v, const std::string &path) {
    if (!v.is_object()) {
      Add(path, "type", "must be an object");
      return;
    }
    for (const auto &[key, value] : v.items()) {
      std::string at = path;
      at += '/';
      at += key;
      const auto sub = schema["properties"].find(key);
      if (sub == schema["properties"].end()) {
        Add(at, "unknown", "isn't a field of this action");
      } else if (!(Empty(value) && Excused(at))) {
        CheckValue(*sub, value, at);
      }
    }
    for (const json &r : schema["required"]) {
      const std::string at = path + "/" + r.get<std::string>();
      if (!v.contains(r.get<std::string>()) && !Excused(at)) {
        Add(at, "missing", "is required");
      }
    }
  }

  void CheckString(const json &schema, const json &v, const std::string &path) {
    if (!v.is_string()) {
      Add(path, "type", "must be text");
      return;
    }
    const auto &s = v.get_ref<const std::string &>();
    if (s.contains('\0')) {
      Add(path, "format", "has a NUL character");
    }
    if (schema.contains("enum") && std::ranges::find(schema["enum"], v) == schema["enum"].end()) {
      Add(path, "enum", "isn't one of the allowed values");
    }
    const std::string format = schema.value("format", "");
    std::int64_t when = 0;
    if (format == "email" && !Email(s)) {
      Add(path, "format", "isn't an email address");
    } else if (format == "date-time" && !ParseDateTime(s, when)) {
      Add(path, "format", "isn't a date and time with an offset");
    }
  }

  // ---- rules a schema can't express (docs/action-schema.md §4) ----

  // The field's text; "" if it's absent or empty.
  std::string Str(const char *key) { return params_->value(key, std::string()); }
  bool Has(const char *key) { return params_->contains(key) && !Empty((*params_)[key]); }
  static std::string At(const char *key) { return std::string("/") + key; }

  void NeedText(const char *key) {
    if (!Has(key) && !Excused(At(key))) {
      Add(At(key), "missing", "can't be empty");
    }
  }
  void OneLine(const char *key, std::size_t max) {
    NeedText(key);
    const std::string s = Str(key);
    // A line break in a subject or title could start a new mail header.
    if (std::ranges::any_of(
            s, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
      Add(At(key), "format", "must be one line");
    }
    AtMost(key, max);
  }
  void AtMost(const char *key, std::size_t max) {
    if (Length(Str(key)) > max) {
      Add(At(key), "too_long", "is longer than " + std::to_string(max) + " characters");
    }
  }
  void RepoField() {
    NeedText("repo");
    if (Has("repo") && !Repo(Str("repo"))) {
      Add("/repo", "format", "must be owner/name");
    }
  }
  void Number() {
    if (Has("number") && (*params_)["number"] < 1) {
      Add("/number", "range", "must be 1 or more");
    }
  }

  void Semantics() {
    const std::string name = action_->connector + "." + action_->type;
    if (name == "email.send_email") {
      NeedText("to");
      OneLine("subject", 998);
      AtMost("body_markdown", 100000);
      std::size_t people = 0;
      for (const char *list : {"to", "cc", "bcc"}) {
        people += Has(list) ? (*params_)[list].size() : 0U;
      }
      if (people > 100) {
        Add("/to", "too_many", "an email can have at most 100 recipients");
      }
    } else if (name == "github.create_issue") {
      RepoField();
      OneLine("title", 256);
      AtMost("body_markdown", 65536);
    } else if (name == "github.comment") {
      RepoField();
      Number();
      NeedText("body_markdown");
      AtMost("body_markdown", 65536);
    } else if (name == "github.review_pr") {
      Review();
    } else if (name == "calendar.create_event") {
      OneLine("title", 1000);
      std::int64_t start = 0;
      std::int64_t end = 0;
      if (Has("start") && Has("end") && ParseDateTime(Str("start"), start) &&
          ParseDateTime(Str("end"), end) && end <= start) {
        Add("/end", "range", "must be after the start");
      }
    } else if (name == "tasks.create_reminder") {
      OneLine("title", 1000);
    }
  }

  void Review() {
    RepoField();
    Number();
    AtMost("summary_markdown", 65536);
    // GitHub refuses an empty review unless it approves.
    if (Has("verdict") && Str("verdict") != "APPROVE" && !Has("summary_markdown") &&
        !Has("comments") && !Excused("/summary_markdown")) {
      Add("/summary_markdown", "missing", "a review needs a summary or a comment");
    }
    if (!Has("comments")) {
      return;
    }
    const json &comments = (*params_)["comments"];
    for (std::size_t i = 0; i < comments.size(); ++i) {
      const json &c = comments[i];
      const std::string at = "/comments/" + std::to_string(i);
      if (Empty(c["body_markdown"])) {
        Add(at + "/body_markdown", "missing", "can't be empty");
      }
      if (c["line"] < 1) {
        Add(at + "/line", "range", "must be 1 or more");
      } else if (diff_ != nullptr) {
        const auto lines =
            diff_->find({c["path"].get<std::string>(), c["side"].get<std::string>()});
        if (lines == diff_->end() || !lines->second.contains(c["line"].get<std::int64_t>())) {
          Add(at + "/line", "not_in_diff", "isn't a line of this pull request's diff");
        }
      }
    }
  }

  const ActionType *action_;
  const json *params_;
  Stage stage_;
  std::set<std::string> missing_;
  const DiffLines *diff_;
  std::vector<FieldError> errors_;
};

std::vector<FieldError> Whole(std::string code, std::string message) {
  return {{.path = "", .code = std::move(code), .message = std::move(message)}};
}

}  // namespace

bool ParseDateTime(std::string_view s, std::int64_t &unix_s) {
  using namespace std::chrono;
  // 2026-11-23T14:05:00[.fff](Z|+hh:mm)
  if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
    return false;
  }
  const int y = Digits(s, 0, 4);
  const int mo = Digits(s, 5, 2);
  const int d = Digits(s, 8, 2);
  const int h = Digits(s, 11, 2);
  const int mi = Digits(s, 14, 2);
  const int sec = Digits(s, 17, 2);
  if (std::min({y, mo, d, h, mi, sec}) < 0 || h > 23 || mi > 59 || sec > 59) {
    return false;
  }
  const year_month_day date{year(y), month(static_cast<unsigned>(mo)),
                            day(static_cast<unsigned>(d))};
  if (!date.ok()) {
    return false;
  }
  std::string_view zone = s.substr(19);
  if (zone.starts_with('.')) {
    const auto end = zone.find_first_not_of("0123456789", 1);
    if (end == 1 || end == std::string_view::npos) {
      return false;
    }
    zone = zone.substr(end);
  }
  std::int64_t offset = 0;
  if (zone != "Z") {
    if (zone.size() != 6 || (zone[0] != '+' && zone[0] != '-') || zone[3] != ':') {
      return false;
    }
    const int oh = Digits(zone, 1, 2);
    const int om = Digits(zone, 4, 2);
    if (oh < 0 || oh > 23 || om < 0 || om > 59) {
      return false;
    }
    offset = ((std::int64_t{oh} * 3600) + (std::int64_t{om} * 60)) * (zone[0] == '-' ? -1 : 1);
  }
  unix_s = (sys_days(date).time_since_epoch() / seconds(1)) + (std::int64_t{h} * 3600) +
           (std::int64_t{mi} * 60) + sec - offset;
  return true;
}

Risk EffectiveRisk(const ActionType &action, const json &params) {
  const bool review = action.connector == "github" && action.type == "review_pr";
  if (review && params.is_object() && params.value("verdict", json()) == "COMMENT") {
    return Risk::kMedium;
  }
  return action.risk;
}

std::vector<FieldError> Validate(const SchemaRegistry &registry, const json &draft, Stage stage,
                                 const DiffLines *diff) {
  if (!draft.is_object()) {
    return Whole("type", "a draft must be an object");
  }
  const auto text = [&](const char *key) {
    const auto it = draft.find(key);
    return it != draft.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  const ActionType *action = registry.Find(text("connector"), text("type"));
  if (action == nullptr) {
    return Whole("unknown_action", "there's no such action");
  }
  if (draft.value("schema_version", json()) != action->version) {
    return Whole("schema_version", "made for another version of this action");
  }
  const auto params = draft.find("params");
  if (params == draft.end()) {
    return Whole("missing", "a draft needs params");
  }
  std::set<std::string> missing;
  if (const auto m = draft.find("missing_fields"); m != draft.end()) {
    if (!m->is_array()) {
      return Whole("type", "missing_fields must be a list");
    }
    for (const json &f : *m) {
      if (!f.is_string() || !action->params_schema["properties"].contains(f.get<std::string>())) {
        return Whole("missing_fields", "missing_fields names a field this action doesn't have");
      }
      missing.insert(f.get<std::string>());
    }
  }
  return Checker(*action, *params, stage, std::move(missing), diff).Run();
}
// GCOVR_EXCL_BR_STOP

}  // namespace micnote::planner
