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
  ASSERT_TRUE(st::SaveTranscript(db, t));

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
  for (const char *m : {"recordings.get@1", "recordings.delete@1", "transcripts.get@1",
                        "items.list@1", "actions.list@1", "actions.get@1"}) {
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

// --- models.list@1, models.select@1 ------------------------------------------

class ModelsContext : public ::testing::Test {
 public:
  ModelsContext() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-models-ctx-" + std::to_string(rd()));
  }
  ModelsContext(const ModelsContext &) = delete;
  ModelsContext &operator=(const ModelsContext &) = delete;
  ModelsContext(ModelsContext &&) = delete;
  ModelsContext &operator=(ModelsContext &&) = delete;
  ~ModelsContext() override {
    ctx_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  void Open(micnote::models::Device device, std::string manifest = "") {
    ctx_.reset();
    micnote::api::ContextOptions o;
    o.models_dir = dir_ / "models";
    o.manifest_json = std::move(manifest);
    o.device = device;
    auto ctx = Context::Open(dir_ / "data", std::make_unique<FileSecrets>(dir_ / "secrets"),
                             nullptr, nullptr, std::move(o));
    ASSERT_TRUE(ctx) << ctx.error().message;
    ctx_ = std::move(*ctx);
  }
  json Ok(const std::string &method, const std::string &params = "{}") {
    const auto r = ctx_->Call(method, params);
    EXPECT_EQ(r.status, MN_OK) << method << " " << params << ": " << r.json;
    return json::parse(r.json);
  }
  mn_status Status(const std::string &method, const std::string &params) {
    return ctx_->Call(method, params).status;
  }
  static const json &ById(const json &list, const std::string &id) {
    for (const auto &m : list["models"]) {
      if (m["id"] == id) {
        return m;
      }
    }
    static const json kNone;
    return kNone;
  }
  [[nodiscard]] const fs::path &dir() const { return dir_; }

 private:
  fs::path dir_;
  std::unique_ptr<Context> ctx_;
};

constexpr micnote::models::Device kBigDesktop{
    .cls = micnote::models::DeviceClass::kDesktop, .ram_mb = 16384, .gpu = true};
constexpr micnote::models::Device kPhone{
    .cls = micnote::models::DeviceClass::kMobile, .ram_mb = 5800, .gpu = false};

TEST_F(ModelsContext, ListsTheBuiltInModelsWithTheDefaults) {
  Open(kBigDesktop);
  const json l = Ok("models.list@1");
  EXPECT_EQ(l["device"]["class"], "desktop");
  EXPECT_EQ(l["device"]["ram_mb"], 16384);
  EXPECT_EQ(l["models_dir"], (dir() / "models").string());
  EXPECT_EQ(l["chosen"]["asr"], "whisper-large-v3-turbo-q5_0");
  EXPECT_EQ(l["chosen"]["vad"], "silero-v6.2.0");
  EXPECT_EQ(l["chosen"]["llm"], nullptr);
  ASSERT_EQ(l["models"].size(), 4U);
  const json &turbo = ById(l, "whisper-large-v3-turbo-q5_0");
  EXPECT_EQ(turbo["kind"], "asr");
  EXPECT_EQ(turbo["file"], "ggml-large-v3-turbo-q5_0.bin");
  EXPECT_EQ(turbo["sha256"], "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2");
  EXPECT_EQ(turbo["classes"], json::array({"desktop"}));
  EXPECT_EQ(turbo["state"], "missing");
  EXPECT_EQ(turbo["default"], true);
  EXPECT_EQ(turbo["chosen"], true);
  EXPECT_EQ(ById(l, "whisper-small.en-q5_1")["default"], false);

  Open(kPhone);
  EXPECT_EQ(Ok("models.list@1")["chosen"]["asr"], "whisper-base.en-q5_1");
}

TEST_F(ModelsContext, StatesComeFromTheFiles) {
  const std::string sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  Open(kBigDesktop,
       R"({"version":1,"models":[
         {"id":"a","kind":"asr","url":"https://x/a.bin","sha256":")" +
           sha + R"(","bytes":3,"classes":["desktop"]},
         {"id":"b","kind":"asr","url":"https://x/b.bin","sha256":")" +
           sha + R"(","bytes":3,"classes":["desktop"]}]})");
  fs::create_directories(dir() / "models");
  std::ofstream(dir() / "models" / "a.bin") << "abc";
  std::ofstream(dir() / "models" / "b.bin") << "abcdef";
  const json l = Ok("models.list@1");
  EXPECT_EQ(ById(l, "a")["state"], "present");
  EXPECT_EQ(ById(l, "b")["state"], "wrong_size");
  EXPECT_EQ(l["chosen"]["asr"], nullptr) << "this manifest has no defaults";
}

TEST_F(ModelsContext, SelectOverridesTheDefaultUntilCleared) {
  Open(kBigDesktop);
  json r = Ok("models.select@1", R"({"kind":"asr","id":"whisper-small.en-q5_1"})");
  EXPECT_EQ(r["chosen"], "whisper-small.en-q5_1");
  json l = Ok("models.list@1");
  EXPECT_EQ(l["chosen"]["asr"], "whisper-small.en-q5_1");
  EXPECT_EQ(ById(l, "whisper-small.en-q5_1")["chosen"], true);
  EXPECT_EQ(ById(l, "whisper-large-v3-turbo-q5_0")["chosen"], false);
  EXPECT_EQ(ById(l, "whisper-large-v3-turbo-q5_0")["default"], true);
  // It's a setting, so it survives a restart.
  Open(kBigDesktop);
  EXPECT_EQ(Ok("models.list@1")["chosen"]["asr"], "whisper-small.en-q5_1");

  r = Ok("models.select@1", R"({"kind":"asr","id":null})");
  EXPECT_EQ(r["chosen"], "whisper-large-v3-turbo-q5_0");
  EXPECT_EQ(Ok("models.list@1")["chosen"]["asr"], "whisper-large-v3-turbo-q5_0");
}

TEST_F(ModelsContext, SelectRefusesWhatDoesntFit) {
  Open(kPhone);
  EXPECT_EQ(Status("models.select@1", R"({"kind":"asr","id":"nope"})"), MN_E_NOT_FOUND);
  EXPECT_EQ(Status("models.select@1", R"({"kind":"vad","id":"whisper-base.en-q5_1"})"),
            MN_E_INVALID)
      << "wrong kind";
  EXPECT_EQ(Status("models.select@1", R"({"kind":"asr","id":"whisper-large-v3-turbo-q5_0"})"),
            MN_E_INVALID)
      << "a desktop model on a phone";
  for (const char *bad : {R"({})", R"({"kind":"asr"})", R"({"kind":"tts","id":null})",
                          R"({"kind":7,"id":null})", R"({"kind":"asr","id":5})"}) {
    EXPECT_EQ(Status("models.select@1", bad), MN_E_INVALID) << bad;
  }
}

// A stored choice the registry no longer offers (a newer manifest dropped
// it, or someone wrote the setting by hand) falls back to the default.
TEST_F(ModelsContext, AStaleChoiceFallsBackToTheDefault) {
  Open(kBigDesktop);
  for (const char *stale : {R"("gone")", "5", R"("silero-v6.2.0")"}) {
    Ok("settings.set@1", std::string(R"({"key":"models.asr","value":)") + stale + "}");
    EXPECT_EQ(Ok("models.list@1")["chosen"]["asr"], "whisper-large-v3-turbo-q5_0") << stale;
  }
}

TEST_F(ModelsContext, StoreErrorsAreReported) {
  Open(kBigDesktop);
  st::Db::FailAfter(1);
  EXPECT_EQ(Status("models.list@1", "{}"), MN_E_IO);
  st::Db::FailAfter(0);
  st::Db::FailAfter(1);
  EXPECT_EQ(Status("models.select@1", R"({"kind":"asr","id":null})"), MN_E_IO);
  st::Db::FailAfter(0);
}

TEST(ContextOpen, ABadManifestIsRefused) {
  std::random_device rd;
  const fs::path dir =
      fs::temp_directory_path() / ("micnote-context-manifest-" + std::to_string(rd()));
  micnote::api::ContextOptions o;
  o.manifest_json = "{";
  auto r = Context::Open(dir / "d", std::make_unique<FileSecrets>(dir / "s"), nullptr, nullptr,
                         std::move(o));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().status, MN_E_INVALID);
  EXPECT_EQ(r.error().code, "manifest");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace
