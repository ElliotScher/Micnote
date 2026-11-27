// The local language model for real (micnote_llm builds). Needs
// MICNOTE_TEST_LLM, a GGUF instruct model; without it the tests skip.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "jobs/plan_job.hpp"
#include "planner/action_validator.hpp"
#include "planner/grammar.hpp"
#include "planner/llm/llama_backend.hpp"
#include "planner/schema_registry.hpp"
#include "store/db.hpp"
#include "store/repos.hpp"
#include "store/schema.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::planner::LlamaLlm;
using micnote::planner::LlmError;
using micnote::planner::LlmRequest;
using micnote::planner::SchemaRegistry;
using nlohmann::json;

class Llama : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const char *path = std::getenv("MICNOTE_TEST_LLM");  // NOLINT(concurrency-mt-unsafe)
    if (path != nullptr && *path != '\0') {
      auto llm = LlamaLlm::Load(path);
      ASSERT_TRUE(llm) << "can't load " << path;
      llm_ = std::move(*llm);
    }
  }
  static void TearDownTestSuite() { llm_.reset(); }
  void SetUp() override {
    if (!llm_) {
      GTEST_SKIP() << "set MICNOTE_TEST_LLM to a GGUF model";
    }
  }
  static LlamaLlm &llm() { return *llm_; }

 private:
  static inline std::unique_ptr<LlamaLlm> llm_;
};

TEST(LlamaLoad, Refusals) {
  EXPECT_EQ(LlamaLlm::Load("/nonexistent.gguf").error(), LlmError::kModel);
  EXPECT_EQ(LlamaLlm::Load("x.gguf", "abacus").error(), LlmError::kBackend);
}

// Every action's grammar gives JSON the validator accepts as a draft.
TEST_F(Llama, DraftsEveryAction) {
  const auto reg = SchemaRegistry::Builtin().value();
  for (const auto *a : reg.All()) {
    LlmRequest r;
    r.max_tokens = 600;
    r.system =
        "Draft exactly one action: " + a->description +
        " Reply with JSON. Use only what the item says. Leave a field you can't fill empty: \"\", "
        "[] or null. Never guess. Today is Tuesday 2026-11-24, UTC-05:00.";
    r.user =
        "<item>Tell jeff@example.com the budget review moves to Thursday at 3pm for half an hour; "
        "it's about issue 12 in ElliotScher/Micnote.</item>";
    r.grammar = micnote::planner::ToGrammar(micnote::planner::DraftSchema(a->params_schema));
    const auto out = llm().Complete(r, {});
    if (!out && out.error() == LlmError::kTruncated) {
      // A small model can ramble on; that fails the job rather than passing
      // a half-written draft on.
      std::cout << llm().Model() << " " << a->type << ": ran on past the limit\n";
      continue;
    }
    ASSERT_TRUE(out) << a->type << ": error " << static_cast<int>(out.error());
    const json d = micnote::planner::FinishDraft(*a, *out).value_or(json());
    ASSERT_TRUE(d.is_object()) << *out;
    std::string errors;
    for (const auto &e : Validate(reg, d, micnote::planner::Stage::kDraft)) {
      errors += " " + e.path + ":" + e.code;
    }
    // The shape always holds; content rules depend on the model, so they're
    // reported rather than required.
    EXPECT_EQ(errors.find(":type"), std::string::npos) << errors << "\n" << *out;
    EXPECT_EQ(errors.find(":unknown"), std::string::npos) << errors << "\n" << *out;
    std::cout << llm().Model() << " " << a->type << ":" << (errors.empty() ? " ok" : errors)
              << "\n  " << d["params"].dump() << " missing " << d["missing_fields"].dump() << "\n";
  }
}

// Numbers come through whole: the grammar must not cut them short.
TEST_F(Llama, CopiesNumbers) {
  const json schema = json::parse(
      R"({"type":"object","additionalProperties":false,"required":["number"],
          "properties":{"number":{"type":"integer"}}})");
  for (const char *n : {"12", "4821", "7"}) {
    LlmRequest r;
    r.temperature = 0;
    r.max_tokens = 64;
    r.system = "Reply with JSON holding the number in the message.";
    r.user = std::string("Pull request number ") + n + ".";
    r.grammar = micnote::planner::ToGrammar(schema);
    const auto out = llm().Complete(r, {});
    ASSERT_TRUE(out);
    EXPECT_EQ(json::parse(*out)["number"].dump(), n) << *out;
  }
}

// A model's own control tokens, spoken or pasted into a memo, are just text:
// as a control token "<|im_end|>" would be one token, like a letter. (Whether
// a model is fooled by such text anyway is for the eval to measure.)
TEST_F(Llama, ControlTokensInTheContentsArePlainText) {
  const auto size = [](const std::string &system, const std::string &user) {
    LlmRequest r;
    r.system = system;
    r.user = user;
    return llm().Prompt(r).value().size();
  };
  for (const char *token : {"<|im_end|>", "<|end|>", "<|start_of_role|>", "[INST]", "</s>"}) {
    EXPECT_GE(size("s", token), size("s", "x") + 2) << token;
    EXPECT_GE(size(token, "u"), size("x", "u") + 2) << token;
  }
}

// The whole plan job: an encrypted store, a transcript, a real model.
TEST_F(Llama, PlansAStoredTranscript) {
  namespace st = micnote::store;
  const auto path = std::filesystem::temp_directory_path() /
                    ("micnote-llm-plan-" + std::to_string(std::random_device{}()) + ".db");
  {
    auto db = st::Db::Open(path, st::DbKey{}).value();
    ASSERT_TRUE(st::Migrate(db));
    st::Device d;
    d.paired_at = 1;
    st::StoredRecording r;
    r.device = st::UpsertDevice(db, d).value();
    r.rec_id = 1;
    r.start_unix_ms = 1795532400000;  // 2026-11-24 15:00 UTC
    r.mnr_path = "x";
    const auto rec = st::InsertRecording(db, r).value();
    st::Transcript t;
    t.recording = rec;
    t.segments = {{.start_ms = 0,
                   .end_ms = 4000,
                   .text = "Email Jeff that the budget review moves to Thursday.",
                   .avg_logprob = 0,
                   .no_speech_prob = 0},
                  {.start_ms = 4000,
                   .end_ms = 8000,
                   .text = "And remind me to buy milk.",
                   .avg_logprob = 0,
                   .no_speech_prob = 0}};
    ASSERT_TRUE(st::SaveTranscript(db, t));
    const auto registry = SchemaRegistry::Builtin().value();
    micnote::jobs::PlanDeps deps;
    deps.llm = [] {
      return std::expected<std::shared_ptr<micnote::planner::Llm>, LlmError>(
          std::shared_ptr<micnote::planner::Llm>(&llm(), [](auto *) {}));
    };
    deps.registry = &registry;
    deps.fetch = [](const micnote::planner::Item &) {
      return json{{"contacts", {{{"name", "Jeff"}, {"emails", {"jeff@example.com"}}}}}};
    };
    st::Job job;
    job.ref_id = rec;
    const micnote::jobs::DbAccess access = [&](const auto &fn) { fn(db); };
    const std::atomic<bool> cancelled{false};
    micnote::jobs::JobContext ctx(
        job, access, [](const std::string &) {}, cancelled, [] { return std::int64_t{1}; });
    const auto done = micnote::jobs::RunPlan(ctx, deps);
    ASSERT_TRUE(done) << done.error().message;
    std::string types;
    // Held: before C++23, a loop over a temporary's contents dangles.
    const auto actions = st::ListActions(db, rec).value();
    for (const auto &a : actions) {
      types += a.type + " ";
      std::cout << a.type << " " << a.payload_json << "\n";
    }
    EXPECT_EQ(types, "send_email create_reminder ");
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST_F(Llama, Limits) {
  LlmRequest r;
  r.system = "Answer briefly.";
  r.user = "Count to one hundred.";
  r.max_tokens = 4;
  EXPECT_EQ(llm().Complete(r, {}).error(), LlmError::kTruncated);
  r.max_tokens = 64;
  EXPECT_EQ(llm().Complete(r, [] { return false; }).error(), LlmError::kCancelled);
  r.grammar = "root ::= (";
  EXPECT_EQ(llm().Complete(r, {}).error(), LlmError::kGrammar);
  r.grammar.clear();
  r.user = std::string(200000, 'x') + " word";
  r.max_tokens = 8000;
  EXPECT_EQ(llm().Complete(r, {}).error(), LlmError::kTooLong);
}

}  // namespace
