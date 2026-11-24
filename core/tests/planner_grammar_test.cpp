// Schemas to llama.cpp grammars (stage 3 §4.3).
#include <gtest/gtest.h>

#include <string>

#include "planner/action_validator.hpp"
#include "planner/grammar.hpp"
#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::planner::DraftSchema;
using micnote::planner::FinishDraft;
using micnote::planner::GbnfLiteral;
using micnote::planner::SchemaRegistry;
using micnote::planner::ToGrammar;
using nlohmann::json;

// The rules for a schema, without the shared ones.
std::string Rules(const char *schema) {
  const std::string g = ToGrammar(json::parse(schema));
  return g.substr(0, g.find("ws ::="));
}

TEST(Grammar, Literals) { EXPECT_EQ(GbnfLiteral(R"("a\b")"), R"("\"a\\b\"")"); }

TEST(Grammar, Scalars) {
  EXPECT_EQ(Rules(R"({"type":"string"})"), "root ::= string\n");
  EXPECT_EQ(Rules(R"({"type":"string","format":"email"})"), "root ::= email\n");
  EXPECT_EQ(Rules(R"({"type":"string","format":"date-time"})"), "root ::= datetime\n");
  EXPECT_EQ(Rules(R"({"type":"integer","x-nullable":true})"), "root ::= (integer | \"null\")\n");
  EXPECT_EQ(Rules(R"({"type":"boolean"})"), "root ::= boolean\n");
  EXPECT_EQ(Rules(R"({"type":"string","enum":["A","B"]})"),
            "root ::= (\"\\\"A\\\"\" | \"\\\"B\\\"\")\n");
}

TEST(Grammar, ListsAndObjects) {
  EXPECT_EQ(Rules(R"({"type":"array","items":{"type":"integer"}})"),
            "root ::= root-list\n"
            "root-list ::= \"[\" ws (integer (\",\" ws integer){0,49})? \"]\"\n");
  EXPECT_EQ(Rules(R"({"type":"object","required":["b_c"],"properties":
              {"a":{"type":"boolean"},"b_c":{"type":"integer"}}})"),
            "root ::= root-object\n"
            "root-object ::= \"{\" ws \"\\\"b_c\\\":\" ws integer (\",\" ws \"\\\"a\\\":\" ws "
            "boolean)? ws \"}\"\n");
  EXPECT_EQ(Rules(R"({"type":"object","required":["a","b"],"properties":
              {"a":{"type":"boolean"},"b":{"type":"boolean"}}})"),
            "root ::= root-object\n"
            "root-object ::= \"{\" ws \"\\\"a\\\":\" ws boolean \",\" ws \"\\\"b\\\":\" ws boolean "
            "ws \"}\"\n");
  EXPECT_EQ(Rules(R"({"type":"object","required":[],"properties":
              {"a":{"type":"boolean"},"b":{"type":"boolean"}}})"),
            "root ::= root-object\n"
            "root-object ::= \"{\" ws (\"\\\"a\\\":\" ws boolean (\",\" ws \"\\\"b\\\":\" ws "
            "boolean)? | \"\\\"b\\\":\" ws boolean)? ws \"}\"\n");
  EXPECT_EQ(Rules(R"({"type":"object","required":[],"properties":{}})"),
            "root ::= root-object\nroot-object ::= \"{\" ws ws \"}\"\n");
}

// What a model can leave blank when it doesn't know, for every action.
TEST(Grammar, DraftsOfEveryAction) {
  const auto reg = SchemaRegistry::Builtin().value();
  for (const auto *a : reg.All()) {
    const json s = DraftSchema(a->params_schema);
    EXPECT_FALSE(ToGrammar(s).empty());
    EXPECT_EQ(s["properties"].size(), 3U) << "params, rationale and confidence only";
  }
  const json review = DraftSchema(reg.Find("github", "review_pr")->params_schema);
  const json &p = review["properties"]["params"]["properties"];
  EXPECT_TRUE(p["number"].contains("x-nullable"));
  EXPECT_TRUE(p["verdict"].contains("x-nullable"));
  EXPECT_FALSE(p["repo"].contains("x-nullable")) << "text can be left empty instead";
  EXPECT_FALSE(p["comments"].contains("x-nullable"));
}

json Finish(const micnote::planner::ActionType &a, const char *output) {
  return FinishDraft(a, output).value_or(json("not a draft"));
}

TEST(Grammar, FinishingADraft) {
  const auto reg = SchemaRegistry::Builtin().value();
  const auto &email = *reg.Find("email", "send_email");
  const json d =
      Finish(email, R"({"params":{"to":[],"subject":"","body_markdown":"Hi","account_id":null},
                 "rationale":"r","confidence":"high","risk":"low","missing_fields":[]})");
  EXPECT_EQ(d["params"], json::parse(R"({"body_markdown":"Hi"})"));
  EXPECT_EQ(d["missing_fields"], json::parse(R"(["to","subject"])"));
  EXPECT_FALSE(d.contains("risk"));
  EXPECT_EQ(d["confidence"], 0.9);
  EXPECT_EQ(d["rationale"], "r");
  EXPECT_TRUE(Validate(reg, d, micnote::planner::Stage::kDraft).empty());
  EXPECT_FALSE(Validate(reg, d, micnote::planner::Stage::kFinal).empty());
  const auto plain = Finish(email, R"({"params":{},"rationale":1,"confidence":"medium"})");
  EXPECT_EQ(plain["confidence"], 0.6);
  EXPECT_EQ(plain["rationale"], "");
  EXPECT_EQ(Finish(email, R"({"params":{}})")["confidence"], 0.3);
  for (const char *bad : {"", "[]", "{}", R"({"params":1})"}) {
    EXPECT_FALSE(FinishDraft(email, bad)) << bad;
  }
}

}  // namespace
