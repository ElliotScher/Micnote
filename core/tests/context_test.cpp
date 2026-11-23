// The C ABI's methods (stage 2 §4.7), through api::Context: every method,
// its results and its errors. Data goes in through a second connection to
// the same encrypted database, as the job queue will put it there.
#include "api/context.hpp"

#include <micnote.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "secrets/secrets.hpp"
#include "store/db.hpp"
#include "store/repos.hpp"
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
namespace st = micnote::store;
using micnote::api::Context;
using micnote::secrets::FileSecrets;
using nlohmann::json;

class ContextTest : public ::testing::Test {
 public:
  ContextTest() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-context-test-" + std::to_string(rd()));
    auto ctx = Context::Open(dir_ / "data", std::make_unique<FileSecrets>(dir_ / "secrets"),
                             &ContextTest::OnEvent, this);
    EXPECT_TRUE(ctx) << ctx.error().message;
    ctx_ = std::move(*ctx);
  }
  ContextTest(const ContextTest &) = delete;
  ContextTest &operator=(const ContextTest &) = delete;
  ContextTest(ContextTest &&) = delete;
  ContextTest &operator=(ContextTest &&) = delete;
  ~ContextTest() override {
    ctx_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  // A second connection, with the key the context stored.
  st::Db Side() {
    FileSecrets s(dir_ / "secrets");
    const auto key = s.Get("db_key").value();
    st::DbKey k{};
    std::ranges::copy(key, k.begin());
    return std::move(st::Db::Open(dir_ / "data" / "micnote.db", k).value());
  }

  struct Result {
    mn_status status;
    json body;
  };
  Result Call(const std::string &method, const std::string &params = "{}") {
    const auto r = ctx_->Call(method, params);
    return Result{.status = r.status, .body = json::parse(r.json)};
  }

  static std::int64_t SeedRecording(st::Db &db, std::int64_t rec_id) {
    st::Device d;
    d.device_id.fill(std::byte{4});
    d.name = "desk";
    d.paired_at = 10;
    const auto dev = st::UpsertDevice(db, d).value();
    st::StoredRecording r;
    r.device = dev;
    r.rec_id = rec_id;
    r.start_unix_ms = rec_id == 1 ? 0 : 1763800000000;
    r.time_confidence = rec_id == 1 ? st::TimeConfidence::kUnknown : st::TimeConfidence::kRtc;
    r.duration_ms = 5000;
    r.mnr_path = "rec/" + std::to_string(rec_id) + ".mnr";
    r.sha256 = {std::byte{0xAB}, std::byte{0x01}};
    r.created_at = 100 + rec_id;
    r.markers = {st::StoredMarker{.offset_ms = 1000, .kind = 1}};
    return st::InsertRecording(db, r).value();
  }

  mn_status CallRaw(const std::string &method, const std::string &params) {
    return ctx_->Call(method, params).status;
  }

  [[nodiscard]] const fs::path &dir() const { return dir_; }
  [[nodiscard]] std::vector<std::string> events() {
    const std::scoped_lock lock(events_mu_);
    return events_;
  }

 private:
  // Events come from the job queue's threads too.
  static void OnEvent(void *ud, const char *event_json) {
    auto *self = static_cast<ContextTest *>(ud);
    const std::scoped_lock lock(self->events_mu_);
    self->events_.emplace_back(event_json);
  }
  fs::path dir_;
  std::unique_ptr<Context> ctx_;
  std::mutex events_mu_;
  std::vector<std::string> events_;
};

TEST_F(ContextTest, EmptyStore) {
  EXPECT_EQ(Call("recordings.list@1").body, json::parse(R"({"recordings":[]})"));
  EXPECT_EQ(Call("devices.list@1").body, json::parse(R"({"devices":[]})"));
}

TEST_F(ContextTest, RecordingsAndDevices) {
  st::Db db = Side();
  const auto a = SeedRecording(db, 1);
  const auto b = SeedRecording(db, 2);

  auto r = Call("recordings.list@1");
  ASSERT_EQ(r.status, MN_OK);
  ASSERT_EQ(r.body["recordings"].size(), 2U);
  EXPECT_EQ(r.body["recordings"][0]["id"], b) << "newest first";
  EXPECT_EQ(r.body["recordings"][0]["start_unix_ms"], 1763800000000);
  EXPECT_EQ(r.body["recordings"][1]["start_unix_ms"], nullptr) << "time unknown";
  EXPECT_EQ(r.body["recordings"][1]["time_confidence"], "unknown");
  EXPECT_EQ(r.body["recordings"][1]["sha256"], "ab01");
  EXPECT_EQ(Call("recordings.list@1", R"({"limit":1})").body["recordings"].size(), 1U);
  // A limit that isn't a count is ignored.
  EXPECT_EQ(Call("recordings.list@1", R"({"limit":-1})").body["recordings"].size(), 2U);
  EXPECT_EQ(Call("recordings.list@1", R"({"limit":"2"})").body["recordings"].size(), 2U);

  r = Call("recordings.get@1", "{\"id\":" + std::to_string(a) + "}");
  ASSERT_EQ(r.status, MN_OK);
  EXPECT_EQ(r.body["rec_id"], 1);
  EXPECT_EQ(r.body["markers"], json::parse(R"([{"offset_ms":1000,"kind":1}])"));
  EXPECT_EQ(r.body["state"], "synced");

  const auto devices = Call("devices.list@1").body["devices"];
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices[0]["device_id"], "0404040404040404");
  EXPECT_EQ(devices[0]["host_slot"], nullptr);
  EXPECT_EQ(devices[0]["last_seen"], nullptr);
  ASSERT_TRUE(st::TouchDevice(db, devices[0]["id"].get<std::int64_t>(), 77, "0.1.0"));
  st::Device d;
  d.device_id.fill(std::byte{4});
  d.host_slot = 1;
  d.paired_at = 10;
  d.last_seen = 77;
  ASSERT_TRUE(st::UpsertDevice(db, d));
  const auto touched = Call("devices.list@1").body["devices"][0];
  EXPECT_EQ(touched["host_slot"], 1);
  EXPECT_EQ(touched["last_seen"], 77);
}

TEST_F(ContextTest, DeleteRemovesTheRowAndTheFile) {
  st::Db db = Side();
  const auto id = SeedRecording(db, 1);
  fs::create_directories(dir() / "data" / "rec");
  std::ofstream(dir() / "data" / "rec" / "1.mnr") << "x";

  const auto r = Call("recordings.delete@1", "{\"id\":" + std::to_string(id) + "}");
  ASSERT_EQ(r.status, MN_OK) << r.body.dump();
  EXPECT_FALSE(fs::exists(dir() / "data" / "rec" / "1.mnr"));
  EXPECT_EQ(Call("recordings.get@1", "{\"id\":" + std::to_string(id) + "}").status, MN_E_NOT_FOUND);
  ASSERT_EQ(events().size(), 1U);
  EXPECT_EQ(json::parse(events()[0])["type"], "recording.deleted");
  EXPECT_EQ(Call("recordings.delete@1", "{\"id\":" + std::to_string(id) + "}").status,
            MN_E_NOT_FOUND);
}

TEST_F(ContextTest, DeleteNeverLeavesTheDataDirectory) {
  st::Db db = Side();
  st::Device d;
  d.device_id.fill(std::byte{5});
  d.paired_at = 1;
  const auto dev = st::UpsertDevice(db, d).value();
  std::ofstream(dir() / "outside") << "keep me";
  for (const auto &path :
       {std::string("../outside"), (dir() / "outside").string(), std::string()}) {
    st::StoredRecording r;
    r.device = dev;
    r.rec_id = static_cast<std::int64_t>(path.size()) + 100;
    r.mnr_path = path;
    r.created_at = 1;
    const auto id = st::InsertRecording(db, r).value();
    EXPECT_EQ(Call("recordings.delete@1", "{\"id\":" + std::to_string(id) + "}").status, MN_OK);
  }
  EXPECT_TRUE(fs::exists(dir() / "outside"));
}

TEST_F(ContextTest, TranscriptsAndSearch) {
  st::Db db = Side();
  const auto rec = SeedRecording(db, 1);
  st::Transcript t;
  t.recording = rec;
  t.model = "small.en";
  t.backend = "cpu";
  t.language = "en";
  t.created_at = 3;
  t.rtf = 0.5;
  t.segments = {st::Segment{.start_ms = 0,
                            .end_ms = 900,
                            .text = "email Jeff about the budget",
                            .avg_logprob = -0.1,
                            .no_speech_prob = 0.0},
                // Invalid UTF-8 comes back replaced, not as an error.
                st::Segment{.start_ms = 900,
                            .end_ms = 1800,
                            .text = "bad \xff byte",
                            .avg_logprob = 0,
                            .no_speech_prob = 0}};
  st::SaveTranscript(db, t).value();

  auto r = Call("transcripts.get@1", "{\"recording\":" + std::to_string(rec) + "}");
  ASSERT_EQ(r.status, MN_OK) << r.body.dump();
  EXPECT_EQ(r.body["model"], "small.en");
  ASSERT_EQ(r.body["segments"].size(), 2U);
  EXPECT_EQ(r.body["segments"][0]["text"], "email Jeff about the budget");
  EXPECT_EQ(r.body["segments"][1]["text"], "bad \xEF\xBF\xBD byte");
  EXPECT_EQ(Call("transcripts.get@1", R"({"recording":999})").status, MN_E_NOT_FOUND);

  r = Call("search.query@1", R"({"q":"budget"})");
  ASSERT_EQ(r.status, MN_OK);
  ASSERT_EQ(r.body["hits"].size(), 1U);
  EXPECT_EQ(r.body["hits"][0]["recording"], rec);
  EXPECT_EQ(Call("search.query@1", R"({"q":"budget","limit":0})").body["hits"].size(), 0U);
  EXPECT_EQ(Call("search.query@1", R"({"q":"\"open"})").status, MN_E_INVALID);
  EXPECT_EQ(Call("search.query@1", R"({"q":""})").status, MN_E_INVALID);
  EXPECT_EQ(Call("search.query@1", R"({"q":3})").status, MN_E_INVALID);
  EXPECT_EQ(Call("search.query@1").status, MN_E_INVALID);
}

TEST_F(ContextTest, Settings) {
  EXPECT_EQ(Call("settings.get@1", R"({"key":"vocab"})").body["value"], nullptr);
  auto r = Call("settings.set@1", R"({"key":"vocab","value":["Jeff","micnote"]})");
  ASSERT_EQ(r.status, MN_OK);
  EXPECT_EQ(Call("settings.get@1", R"({"key":"vocab"})").body["value"],
            json::parse(R"(["Jeff","micnote"])"));
  EXPECT_EQ(Call("settings.get@1", R"({"key":1})").status, MN_E_INVALID);
  EXPECT_EQ(Call("settings.get@1").status, MN_E_INVALID);
  EXPECT_EQ(Call("settings.set@1", R"({"key":"x"})").status, MN_E_INVALID);
  EXPECT_EQ(Call("settings.set@1", R"({"value":1})").status, MN_E_INVALID);
  EXPECT_EQ(Call("settings.set@1", R"({"key":2,"value":1})").status, MN_E_INVALID);
}

TEST_F(ContextTest, BadRequests) {
  auto r = Call("recordings.get@1", R"({"id":"seven"})");
  EXPECT_EQ(r.status, MN_E_INVALID);
  EXPECT_EQ(r.body["error"]["code"], "invalid_params");
  EXPECT_EQ(Call("recordings.get@1").status, MN_E_INVALID);
  EXPECT_EQ(Call("recordings.delete@1").status, MN_E_INVALID);
  EXPECT_EQ(Call("transcripts.get@1").status, MN_E_INVALID);
  EXPECT_EQ(Call("recordings.list@1", "[1]").status, MN_E_INVALID);
  EXPECT_EQ(Call("recordings.list@1", "{oops").status, MN_E_INVALID);
  r = Call("recordings.list@2");
  EXPECT_EQ(r.status, MN_E_INVALID);
  EXPECT_EQ(r.body["error"]["code"], "unknown_method");
  // Empty params mean {}.
  EXPECT_EQ(CallRaw("recordings.list@1", ""), MN_OK);
}

TEST_F(ContextTest, StoreErrorsAreReportedNotThrown) {
  // Every method that reads the store, with its first database call failing.
  for (const char *m : {"devices.list@1", "recordings.list@1", "settings.get@1", "settings.set@1",
                        "search.query@1"}) {
    st::Db::FailAfter(1);
    const auto r = Call(m, R"({"key":"k","value":1,"q":"x","id":1,"recording":1})");
    st::Db::FailAfter(0);
    EXPECT_NE(r.status, MN_OK) << m;
    EXPECT_TRUE(r.body.contains("error")) << m;
  }
  for (const char *m : {"recordings.get@1", "recordings.delete@1", "transcripts.get@1"}) {
    st::Db::FailAfter(1);
    EXPECT_EQ(Call(m, R"({"id":1,"recording":1})").status, MN_E_IO) << m;
    st::Db::FailAfter(0);
  }
  // Delete failing at each of its database calls after the lookup.
  st::Db db = Side();
  bool deleted = false;
  for (int n = 1; n < 30 && !deleted; ++n) {
    const auto id = SeedRecording(db, 100 + n);
    st::Db::FailAfter(n);
    const auto r = Call("recordings.delete@1", "{\"id\":" + std::to_string(id) + "}");
    deleted = !st::Db::FaultPending() && r.status == MN_OK;
    const bool untouched = st::Db::FaultPending();
    st::Db::FailAfter(0);
    if (untouched) {
      EXPECT_EQ(r.status, MN_OK);
      break;
    }
    EXPECT_TRUE(r.status == MN_E_IO || r.status == MN_OK) << n;
  }
}

TEST_F(ContextTest, ACorruptSettingIsAnInternalErrorNotACrash) {
  st::Db db = Side();
  ASSERT_TRUE(st::SetSetting(db, "broken", "{not json"));
  const auto r = Call("settings.get@1", R"({"key":"broken"})");
  EXPECT_EQ(r.status, MN_E_INTERNAL);
  EXPECT_EQ(r.body["error"]["code"], "internal");
}

TEST(ContextOpen, Failures) {
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("micnote-context-open-" + std::to_string(rd()));
  fs::create_directories(dir);
  std::ofstream(dir / "file") << "x";

  // data_dir is a file.
  auto r = Context::Open(dir / "file", std::make_unique<FileSecrets>(dir / "s"), nullptr, nullptr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().status, MN_E_IO);

  // The secret store can't hold db_key.
  r = Context::Open(dir / "d1", std::make_unique<FileSecrets>(dir / "file"), nullptr, nullptr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().status, MN_E_CRYPTO);

  // micnote.db isn't a database at all, or is from a newer build.
  fs::create_directories(dir / "d2" / "micnote.db");
  r = Context::Open(dir / "d2", std::make_unique<FileSecrets>(dir / "s2"), nullptr, nullptr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().status, MN_E_IO);

  {
    auto ok =
        Context::Open(dir / "d3", std::make_unique<FileSecrets>(dir / "s3"), nullptr, nullptr);
    ASSERT_TRUE(ok);
  }
  {
    FileSecrets s(dir / "s3");
    st::DbKey k{};
    std::ranges::copy(s.Get("db_key").value(), k.begin());
    auto db = st::Db::Open(dir / "d3" / "micnote.db", k);
    ASSERT_TRUE(db->Exec("PRAGMA user_version = 99;"));
  }
  r = Context::Open(dir / "d3", std::make_unique<FileSecrets>(dir / "s3"), nullptr, nullptr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, "store");

  // The C config without its required parts.
  const mn_config empty{};
  EXPECT_EQ(Context::Open(empty).error().status, MN_E_INVALID);
  const auto get = [](void *, const char *, std::uint8_t *, std::size_t, std::size_t *) {
    return 1;
  };
  const auto set = [](void *, const char *, const std::uint8_t *, std::size_t) { return 0; };
  const std::string d = (dir / "d4").string();
  EXPECT_EQ(Context::Open(mn_config{.data_dir = d.c_str(),
                                    .models_dir = nullptr,
                                    .get_secret = nullptr,
                                    .set_secret = set,
                                    .on_event = nullptr,
                                    .user_data = nullptr})
                .error()
                .status,
            MN_E_INVALID);
  EXPECT_EQ(Context::Open(mn_config{.data_dir = d.c_str(),
                                    .models_dir = nullptr,
                                    .get_secret = get,
                                    .set_secret = nullptr,
                                    .on_event = nullptr,
                                    .user_data = nullptr})
                .error()
                .status,
            MN_E_INVALID);

  // No event callback: events are dropped.
  {
    auto quiet =
        Context::Open(dir / "d5", std::make_unique<FileSecrets>(dir / "s5"), nullptr, nullptr);
    ASSERT_TRUE(quiet);
    (*quiet)->Emit(R"({"type":"nobody.listens"})");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace
