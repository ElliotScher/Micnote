// Action schemas: the built-in connectors, what a definition may contain, and
// validating drafts (the schema, then the rules it can't express).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "planner/action_validator.hpp"
#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::planner::CheckSchema;
using micnote::planner::DiffLines;
using micnote::planner::EffectiveRisk;
using micnote::planner::FieldError;
using micnote::planner::ParseDateTime;
using micnote::planner::Risk;
using micnote::planner::SchemaRegistry;
using micnote::planner::Stage;
using micnote::planner::Validate;
using nlohmann::json;

SchemaRegistry Builtin() {
  auto reg = SchemaRegistry::Builtin();
  EXPECT_TRUE(reg) << reg.error();
  return std::move(*reg);
}

// ---- the registry -------------------------------------------------------------

TEST(Schemas, BuiltinActions) {
  const auto reg = Builtin();
  std::string names;
  for (const auto *a : reg.All()) {
    names += a->connector + "." + a->type + ":" + std::string(ToString(a->risk)) + " ";
  }
  EXPECT_EQ(names,
            "calendar.create_event:medium email.send_email:high github.comment:medium "
            "github.create_issue:medium github.review_pr:high tasks.create_reminder:low ");
  const auto *email = reg.Find("email", "send_email");
  ASSERT_NE(email, nullptr);
  EXPECT_EQ(email->version, 1);
  EXPECT_EQ(email->context_fetchers,
            (std::vector<std::string>{"contacts.lookup", "email.accounts"}));
  EXPECT_EQ(reg.Find("email", "delete_everything"), nullptr);
}

json Definition(const std::string &connector = "c", int version = 1) {
  return {
      {"connector", connector},
      {"version", version},
      {"actions",
       {{{"type", "act"},
         {"description", "d"},
         {"risk", "low"},
         {"context_fetchers", json::array()},
         {"params_schema",
          {{"type", "object"},
           {"additionalProperties", false},
           {"required", {"flag"}},
           {"properties", {{"flag", {{"type", "boolean"}}}, {"n", {{"type", "integer"}}}}}}}}}}};
}

TEST(Schemas, VersionsReplaceButNeverGoBack) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.Add(Definition("c", 2).dump()));
  ASSERT_TRUE(reg.Add(Definition("other").dump()));
  json renamed = Definition("c", 2);
  renamed["actions"][0]["type"] = "act2";
  ASSERT_TRUE(reg.Add(renamed.dump())) << "the same version again replaces it";
  EXPECT_EQ(reg.Find("c", "act"), nullptr);
  EXPECT_NE(reg.Find("c", "act2"), nullptr);
  EXPECT_NE(reg.Find("other", "act"), nullptr);
  EXPECT_FALSE(reg.Add(Definition("c", 1).dump()));
  EXPECT_NE(reg.Find("c", "act2"), nullptr) << "a refused definition changes nothing";
}

TEST(Schemas, BadDefinitionsAreRefused) {
  const auto refused = [](const json &d) {
    SchemaRegistry reg;
    const auto r = reg.Add(d.dump());
    return !r && !r.error().empty() && reg.All().empty();
  };
  EXPECT_FALSE(refused(Definition()));
  SchemaRegistry reg;
  EXPECT_FALSE(reg.Add("{"));
  EXPECT_TRUE(refused(json::array()));
  const std::vector<std::pair<std::string, json>> cases = {
      {"/extra", 1},
      {"/connector", 3},
      {"/connector", ""},
      {"/connector", "No Spaces"},
      {"/connector", std::string(65, 'a')},
      {"/version", 0},
      {"/version", "1"},
      {"/version", 1000001},
      {"/actions", json::array()},
      {"/actions", 1},
      {"/actions/0", 1},
      {"/actions/0/extra", 1},
      {"/actions/0/type", "a/b"},
      {"/actions/0/type", "a~"},
      {"/actions/0/type", "a:"},
      {"/actions/0/params_schema/properties/n/minimum", 1},
      {"/actions/0/type", 1},
      {"/actions/0/description", 1},
      {"/actions/0/risk", "none"},
      {"/actions/0/risk", 1},
      {"/actions/0/context_fetchers", 1},
      {"/actions/0/context_fetchers", {1}},
      {"/actions/0/params_schema", 1},
      {"/actions/0/params_schema", {{"type", "string"}}},
      {"/actions/0/params_schema/minProperties", 1},
      {"/actions/0/params_schema/additionalProperties", true},
      {"/actions/0/params_schema/properties", 1},
      {"/actions/0/params_schema/required", 1},
      {"/actions/0/params_schema/required", {"nope"}},
      {"/actions/0/params_schema/properties/Bad Name", {{"type", "string"}}}};
  for (const auto &[pointer, value] : cases) {
    json d = Definition();
    d[json::json_pointer(pointer)] = value;
    EXPECT_TRUE(refused(d)) << pointer << " = " << value;
  }
  for (const char *key : {"connector", "version", "actions"}) {
    json d = Definition();
    d.erase(key);
    EXPECT_TRUE(refused(d)) << key;
  }
  for (const char *key : {"type", "description", "risk", "context_fetchers", "params_schema"}) {
    json d = Definition();
    d["actions"][0].erase(key);
    EXPECT_TRUE(refused(d)) << key;
  }
  for (const char *key : {"additionalProperties", "required", "properties"}) {
    json d = Definition();
    d["actions"][0]["params_schema"].erase(key);
    EXPECT_TRUE(refused(d)) << key;
  }
  json twice = Definition();
  twice["actions"].push_back(twice["actions"][0]);
  EXPECT_TRUE(refused(twice));
}

// What llama.cpp grammars and Claude's structured outputs can't both take.
TEST(Schemas, OnlyTheSupportedSubset) {
  for (const char *good :
       {R"({"type":"string","description":"x","enum":["a"]})",
        R"({"type":"string","format":"email"})", R"({"type":"string","format":"date-time"})",
        R"({"type":"array","items":{"type":"integer"}})", R"({"type":"boolean"})"}) {
    EXPECT_EQ(CheckSchema(json::parse(good)), "") << good;
  }
  for (const char *bad :
       {R"(1)", R"({})", R"({"type":1})", R"({"type":["string","null"]})", R"({"type":"number"})",
        R"({"type":"string","maxLength":5})", R"({"type":"string","pattern":"x"})",
        R"({"type":"string","description":1})", R"({"type":"string","enum":[]})",
        R"({"type":"string","enum":[1]})", R"({"type":"string","enum":"a"})",
        R"({"type":"string","format":"uri"})", R"({"type":"string","format":1})",
        R"({"type":"integer","minimum":1})", R"({"type":"array"})",
        R"({"type":"array","items":{"type":"string"},"minItems":1})", R"({"$ref":"#"})"}) {
    EXPECT_NE(CheckSchema(json::parse(bad)), "") << bad;
  }
  json deep = {{"type", "string"}};
  for (int i = 0; i < 7; ++i) {
    deep = {{"type", "array"}, {"items", deep}};
  }
  EXPECT_EQ(CheckSchema(deep), "");
  EXPECT_NE(CheckSchema({{"type", "array"}, {"items", deep}}), "");
}

// ---- drafts -------------------------------------------------------------------

json Draft(const std::string &connector, const std::string &type, const json &params) {
  return {{"connector", connector}, {"type", type}, {"schema_version", 1}, {"params", params}};
}
json Email() {
  return Draft("email", "send_email",
               {{"to", {"jeff@example.com"}}, {"subject", "Budget"}, {"body_markdown", "Hi"}});
}
json Review() {
  return Draft("github", "review_pr",
               {{"repo", "a/b"},
                {"number", 12},
                {"verdict", "COMMENT"},
                {"summary_markdown", "Looks fine"},
                {"comments",
                 {{{"path", "x.c"}, {"line", 3}, {"side", "RIGHT"}, {"body_markdown", "why?"}}}}});
}
json Event() {
  return Draft(
      "calendar", "create_event",
      {{"title", "Sync"}, {"start", "2026-11-26T15:00:00-05:00"}, {"end", "2026-11-26T21:00:00Z"}});
}

// "path:code" of each error, space-separated.
std::string Errors(const json &draft, Stage stage = Stage::kFinal,
                   const DiffLines *diff = nullptr) {
  const auto reg = Builtin();
  std::string out;
  for (const FieldError &e : Validate(reg, draft, stage, diff)) {
    EXPECT_FALSE(e.message.empty());
    out += (out.empty() ? "" : " ") + e.path + ":" + e.code;
  }
  return out;
}

// draft with the value at a JSON pointer into params replaced.
std::string With(json draft, const std::string &pointer, const json &value) {
  draft["params"][json::json_pointer(pointer)] = value;
  return Errors(draft);
}

TEST(Drafts, GoodOnesPass) {
  EXPECT_EQ(Errors(Email()), "");
  EXPECT_EQ(Errors(Review()), "");
  EXPECT_EQ(Errors(Event()), "");
  EXPECT_EQ(Errors(Draft("github", "create_issue",
                         {{"repo", "Aa/Zb.c-d_1"}, {"title", "T"}, {"body_markdown", ""}})),
            "");
  EXPECT_EQ(
      Errors(Draft("github", "comment", {{"repo", "a/b"}, {"number", 1}, {"body_markdown", "x"}})),
      "");
  EXPECT_EQ(Errors(Draft("tasks", "create_reminder", {{"title", "Milk"}})), "");
}

TEST(Drafts, TheEnvelope) {
  EXPECT_EQ(Errors(json::array()), ":type");
  EXPECT_EQ(Errors(Draft("email", "wipe_inbox", json::object())), ":unknown_action");
  EXPECT_EQ(Errors(json::object()), ":unknown_action");
  json d = Email();
  d["schema_version"] = 2;
  EXPECT_EQ(Errors(d), ":schema_version");
  d = Email();
  d.erase("params");
  EXPECT_EQ(Errors(d), ":missing");
  d = Email();
  d["params"] = "x";
  EXPECT_EQ(Errors(d), ":type");
  d = Email();
  d["missing_fields"] = 1;
  EXPECT_EQ(Errors(d), ":type");
  d["missing_fields"] = {"nope"};
  EXPECT_EQ(Errors(d), ":missing_fields");
  d["missing_fields"] = {1};
  EXPECT_EQ(Errors(d), ":missing_fields");
}

TEST(Drafts, TheSchema) {
  EXPECT_EQ(With(Email(), "/to", "jeff@example.com"), "/to:type");
  EXPECT_EQ(With(Email(), "/to/0", 1), "/to/0:type");
  EXPECT_EQ(With(Email(), "/surprise", 1), "/surprise:unknown");
  EXPECT_EQ(With(Email(), "/subject", std::string("a\0b", 3)), "/subject:format");
  EXPECT_EQ(With(Review(), "/number", "12"), "/number:type");
  EXPECT_EQ(With(Review(), "/number", 1.5), "/number:type");
  EXPECT_EQ(With(Review(), "/verdict", "MERGE"), "/verdict:enum");
  EXPECT_EQ(With(Review(), "/comments/0", 1), "/comments/0:type");
  json d = Email();
  d["params"].erase("subject");
  EXPECT_EQ(Errors(d), "/subject:missing");
  SchemaRegistry reg;
  ASSERT_TRUE(reg.Add(Definition().dump()));
  EXPECT_TRUE(Validate(reg, Draft("c", "act", {{"flag", true}, {"n", 3}}), Stage::kFinal).empty());
  EXPECT_EQ(Validate(reg, Draft("c", "act", {{"flag", 1}}), Stage::kFinal).at(0).code, "type");
}

TEST(Drafts, EmailAddresses) {
  for (const char *good : {"a@b.co", "first.last+tag@sub.example.com"}) {
    EXPECT_EQ(With(Email(), "/to/0", good), "") << good;
  }
  for (const char *bad :
       {"", "jeff", "@b.co", "a@b", "a@@b.co", "a@.co", "a@b.", "a@b..co", "a b@c.co",
        "a@b.co\r\nBcc: x@y.zz", "a@b.co,c@d.co", "Jeff <a@b.co>", "a@b.co\x7f"}) {
    EXPECT_EQ(With(Email(), "/to/0", bad), "/to/0:format") << bad;
  }
  EXPECT_EQ(With(Email(), "/to/0", std::string(250, 'a') + "@b.co"), "/to/0:format");
}

// What the planner couldn't fill is excused in a draft, never at execution.
TEST(Drafts, MissingFields) {
  json d = Email();
  d["params"]["to"] = json::array();
  d["params"].erase("subject");
  d["missing_fields"] = {"to", "subject"};
  EXPECT_EQ(Errors(d, Stage::kDraft), "");
  EXPECT_EQ(Errors(d, Stage::kFinal), "/subject:missing");
  d["params"]["subject"] = "S";
  EXPECT_EQ(Errors(d, Stage::kFinal), "/to:missing");
  d["params"]["to"] = nullptr;
  EXPECT_EQ(Errors(d, Stage::kDraft), "");
  d["params"]["to"] = {"not an address"};
  EXPECT_EQ(Errors(d, Stage::kDraft), "/to/0:format") << "filled in, so it's checked";
  d = Email();
  d["params"]["subject"] = "";
  EXPECT_EQ(Errors(d, Stage::kDraft), "/subject:missing") << "empty and not declared";
}

TEST(Drafts, EmailRules) {
  EXPECT_EQ(With(Email(), "/subject", "a\r\nBcc: x@y.zz"), "/subject:format");
  EXPECT_EQ(With(Email(), "/subject", "a\x7f"), "/subject:format");
  EXPECT_EQ(With(Email(), "/subject", std::string(999, 'x')), "/subject:too_long");
  std::string accents;
  for (int i = 0; i < 998; ++i) {
    accents += "\xc3\xa9";
  }
  EXPECT_EQ(With(Email(), "/subject", accents), "") << "characters, not bytes";
  EXPECT_EQ(With(Email(), "/body_markdown", std::string(100001, 'x')), "/body_markdown:too_long");
  json many = json::array();
  for (int i = 0; i < 100; ++i) {
    many.push_back("a" + std::to_string(i) + "@b.co");
  }
  EXPECT_EQ(With(Email(), "/bcc", many), "/to:too_many");
}

TEST(Drafts, GithubRules) {
  for (const char *bad :
       {"ab", "a/", "/b", "a/..", "./b", "a/b/c", "a/b?x=1", "a/b c", "a/b~", "a/b%", "a/b["}) {
    EXPECT_EQ(With(Review(), "/repo", bad), "/repo:format") << bad;
  }
  EXPECT_EQ(With(Review(), "/repo", ""), "/repo:missing");
  EXPECT_EQ(With(Review(), "/repo", "a/" + std::string(101, 'x')), "/repo:format");
  EXPECT_EQ(With(Review(), "/number", 0), "/number:range");
  EXPECT_EQ(With(Review(), "/comments/0/line", 0), "/comments/0/line:range");
  EXPECT_EQ(With(Review(), "/comments/0/body_markdown", ""), "/comments/0/body_markdown:missing");
  json d = Review();
  d["params"]["comments"] = json::array();
  EXPECT_EQ(Errors(d), "");
  d["params"]["summary_markdown"] = "";
  EXPECT_EQ(Errors(d), "/summary_markdown:missing");
  d["params"]["verdict"] = "APPROVE";
  EXPECT_EQ(Errors(d), "") << "an approval needs no words";
}

TEST(Drafts, ReviewCommentsMustBeOnTheDiff) {
  const DiffLines diff = {{{"x.c", "RIGHT"}, {1, 2, 3}}};
  EXPECT_EQ(Errors(Review(), Stage::kFinal, &diff), "");
  json d = Review();
  d["params"]["comments"][0]["line"] = 4;
  EXPECT_EQ(Errors(d, Stage::kDraft, &diff), "/comments/0/line:not_in_diff");
  d["params"]["comments"][0]["line"] = 3;
  d["params"]["comments"][0]["side"] = "LEFT";
  EXPECT_EQ(Errors(d, Stage::kDraft, &diff), "/comments/0/line:not_in_diff");
}

TEST(Drafts, EventsEndAfterTheyStart) {
  EXPECT_EQ(With(Event(), "/end", "2026-11-26T15:30:00-05:00"), "");
  EXPECT_EQ(With(Event(), "/end", "2026-11-26T20:00:00Z"), "/end:range") << "the same instant";
  EXPECT_EQ(With(Event(), "/start", "tomorrow"), "/start:format");
}

TEST(Drafts, DateTimes) {
  std::int64_t t = 0;
  ASSERT_TRUE(ParseDateTime("1970-01-02T00:00:01Z", t));
  EXPECT_EQ(t, 86401);
  ASSERT_TRUE(ParseDateTime("1970-01-01T01:30:00.250+01:30", t));
  EXPECT_EQ(t, 0);
  ASSERT_TRUE(ParseDateTime("2028-02-29T00:00:00-00:30", t));
  for (const char *bad : {"",
                          "2026-11-26",
                          "2026/11/26T00:00:00Z",
                          "2026-11/26T00:00:00Z",
                          "2026-11-26 00:00:00Z",
                          "2026-11-26T00.00:00Z",
                          "2026-11-26T00:00.00Z",
                          "2026-11-26T00:00:00",
                          "2026-11-26T00:00:00z",
                          "2026-11-26T00:00:00.Z",
                          "2026-11-26T00:00:00.5",
                          "2026-11-26T00:00:00+0530",
                          "2026-11-26T00:00:00+05-30",
                          "2026-11-26T00:00:00*05:30",
                          "2026-11-26T00:00:00+24:00",
                          "2026-11-26T00:00:00+00:60",
                          "2026-11-26T00:00:00+x0:00",
                          "2026-11-26T00:00:00+00:x0",
                          "2026-02-30T00:00:00Z",
                          "2026-13-01T00:00:00Z",
                          "2026-11-26T24:00:00Z",
                          "2026-11-26T00:60:00Z",
                          "2026-11-26T00:00:60Z",
                          "2026-11-2xT00:00:00Z",
                          "2026-11-2 T00:00:00Z"}) {
    EXPECT_FALSE(ParseDateTime(bad, t)) << bad;
  }
}

// A draft's own idea of its risk is ignored.
TEST(Drafts, RiskComesFromTheRegistry) {
  const auto reg = Builtin();
  const auto &review = *reg.Find("github", "review_pr");
  EXPECT_EQ(EffectiveRisk(review, Review()["params"]), Risk::kMedium);
  EXPECT_EQ(EffectiveRisk(review, {{"verdict", "APPROVE"}}), Risk::kHigh);
  EXPECT_EQ(EffectiveRisk(review, {{"verdict", "REQUEST_CHANGES"}}), Risk::kHigh);
  EXPECT_EQ(EffectiveRisk(review, {{"risk", "low"}}), Risk::kHigh);
  EXPECT_EQ(EffectiveRisk(review, json()), Risk::kHigh);
  EXPECT_EQ(EffectiveRisk(*reg.Find("email", "send_email"), {{"verdict", "COMMENT"}}), Risk::kHigh);
}

}  // namespace
