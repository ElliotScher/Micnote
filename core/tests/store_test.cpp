// The encrypted store: opening with the right and wrong key, the file being
// ciphertext, migrations, every repository function, and (by fault
// injection) every way each of them can fail.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "store/db.hpp"
#include "store/repos.hpp"
#include "store/schema.hpp"
#include <sqlite3.h>

namespace {

namespace fs = std::filesystem;
using micnote::store::ClockObservation;
using micnote::store::Db;
using micnote::store::DbKey;
using micnote::store::Device;
using micnote::store::DeviceId;
using micnote::store::Migration;
using micnote::store::RecordingState;
using micnote::store::Segment;
using micnote::store::StoredMarker;
using micnote::store::StoredRecording;
using micnote::store::TimeConfidence;
using micnote::store::Transcript;
using micnote::store::Txn;
namespace st = micnote::store;

// The value of an optional the test expects to be there; a missing one
// fails the test (gtest reports the exception).
template <typename T>
T Present(const std::optional<T> &o) {
  if (!o.has_value()) {
    throw std::runtime_error("expected a value, got none");
  }
  return *o;
}

DbKey Key(std::uint8_t fill) {
  DbKey k{};
  k.fill(std::byte{fill});
  return k;
}

class StoreTest : public ::testing::Test {
 public:
  StoreTest() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-store-test-" + std::to_string(rd()));
    fs::create_directories(dir_);
  }
  StoreTest(const StoreTest &) = delete;
  StoreTest &operator=(const StoreTest &) = delete;
  StoreTest(StoreTest &&) = delete;
  StoreTest &operator=(StoreTest &&) = delete;
  ~StoreTest() override {
    Db::FailAfter(0);
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  [[nodiscard]] fs::path path() const { return dir_ / "micnote.db"; }
  [[nodiscard]] const fs::path &dir_path() const { return dir_; }

  Db Open() {
    auto db = Db::Open(path(), Key(0x11));
    EXPECT_TRUE(db) << db.error().message;
    EXPECT_TRUE(st::Migrate(*db));
    return std::move(*db);
  }

  static std::int64_t AddDevice(Db &db, std::uint8_t id = 1) {
    Device d;
    d.device_id.fill(std::byte{id});
    d.name = "puck";
    d.paired_at = 1000;
    return st::UpsertDevice(db, d).value();
  }

  static StoredRecording Recording(std::int64_t device, std::int64_t rec_id) {
    StoredRecording r;
    r.device = device;
    r.rec_id = rec_id;
    r.start_boot = 3;
    r.start_uptime_ms = 5000;
    r.start_unix_ms = 0;
    r.time_confidence = TimeConfidence::kUnknown;
    r.duration_ms = 12000;
    r.mnr_path = "rec/" + std::to_string(rec_id) + ".mnr";
    r.sha256 = std::vector<std::byte>(32, std::byte{0xAB});
    r.created_at = 2000 + rec_id;
    r.markers = {StoredMarker{.offset_ms = 1000, .kind = 1},
                 StoredMarker{.offset_ms = 3000, .kind = 1}};
    return r;
  }

 private:
  fs::path dir_;
};

// Sweeps fault injection through `op` (on a fresh migrated database each
// time): failing the 1st, 2nd, ... database call it makes. Each injected
// fault must make it fail, unless it lands on a call whose error `op` itself
// ignores; the sweep ends when `op` makes fewer calls than the fault waits
// for, and must then succeed.
void Sweep(const std::function<Db()> &open, const std::function<bool(Db &)> &op) {
  int n = 1;
  for (; n < 200; ++n) {
    Db db = open();
    Db::FailAfter(n);
    const bool ok = op(db);
    const bool reached_end = Db::FaultPending();
    Db::FailAfter(0);
    if (reached_end) {
      EXPECT_TRUE(ok) << "fails with no fault injected";
      break;
    }
  }
  EXPECT_GT(n, 1) << "the operation made no database calls";
  EXPECT_LT(n, 200) << "the operation never ran out of calls";
}

// ---- Db ---------------------------------------------------------------------

TEST_F(StoreTest, ReopensWithTheKeyAndRefusesAnother) {
  {
    Db db = Open();
    ASSERT_TRUE(st::SetSetting(db, "k", "\"v\""));
  }
  auto again = Db::Open(path(), Key(0x11));
  ASSERT_TRUE(again);
  EXPECT_EQ(st::GetSetting(*again, "k").value(), "\"v\"");

  auto wrong = Db::Open(path(), Key(0x22));
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().code, SQLITE_NOTADB) << wrong.error().message;
}

TEST_F(StoreTest, TheFileIsCiphertext) {
  {
    Db db = Open();
    ASSERT_TRUE(st::SetSetting(db, "secret-marker", "\"plainly visible\""));
    ASSERT_TRUE(db.Exec("PRAGMA wal_checkpoint(TRUNCATE);"));
  }
  std::ifstream f(path(), std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(f), {}};
  ASSERT_GT(bytes.size(), 4096U);
  EXPECT_EQ(bytes.find("SQLite format 3"), std::string::npos);
  EXPECT_EQ(bytes.find("plainly visible"), std::string::npos);
  EXPECT_EQ(bytes.find("CREATE TABLE"), std::string::npos);
}

TEST_F(StoreTest, OpenFailures) {
  // A directory where the file should be.
  fs::create_directories(path());
  EXPECT_FALSE(Db::Open(path(), Key(1)));
  fs::remove(path());

  // Each of Open's own calls failing (keying, the check read, the pragmas).
  for (int n = 1; n <= 3; ++n) {
    Db::FailAfter(n);
    EXPECT_FALSE(Db::Open(path(), Key(1))) << n;
    Db::FailAfter(0);
  }
  EXPECT_TRUE(Db::Open(path(), Key(1)));
}

TEST_F(StoreTest, StatementsAndErrors) {
  Db db = Open();
  EXPECT_FALSE(db.Exec("NOT SQL"));
  EXPECT_FALSE(db.Prepare("SELECT * FROM nowhere"));

  auto s = db.Prepare("SELECT ?, ?, ?, ?, ?, ?");
  ASSERT_TRUE(s);
  const std::array<std::byte, 2> blob{std::byte{1}, std::byte{2}};
  s->Bind(1, std::int64_t{-7})
      .Bind(2, 2.5)
      .Bind(3, std::string_view("text"))
      .Bind(4, std::span<const std::byte>(blob))
      .BindNull(5)
      .Bind(6, std::span<const std::byte>());
  ASSERT_TRUE(s->Step().value());
  EXPECT_EQ(s->Int(0), -7);
  EXPECT_EQ(s->Real(1), 2.5);
  EXPECT_EQ(s->Text(2), "text");
  EXPECT_EQ(s->Blob(3), std::vector<std::byte>(blob.begin(), blob.end()));
  EXPECT_TRUE(s->IsNull(4));
  EXPECT_EQ(s->OptInt(4), std::nullopt);
  EXPECT_EQ(s->Text(4), "");
  EXPECT_TRUE(s->Blob(4).empty());
  EXPECT_FALSE(s->IsNull(5));
  EXPECT_FALSE(s->Step().value());

  // A bad index is reported by the next step (the first of several), and
  // Reset clears it.
  s->Reset();
  s->Bind(9, std::int64_t{1}).Bind(10, 2.0);
  EXPECT_FALSE(s->Step());
  s->Reset();
  EXPECT_TRUE(s->Step());

  // Run steps through rows too.
  auto rows = db.Prepare("SELECT 1 UNION ALL SELECT 2");
  EXPECT_TRUE(rows->Run());

  // A constraint error from Step.
  ASSERT_TRUE(db.Exec("CREATE TABLE t (x INTEGER UNIQUE);"));
  auto ins = db.Prepare("INSERT INTO t VALUES (1)");
  ASSERT_TRUE(ins->Run());
  ins->Reset();
  auto dup = ins->Run();
  ASSERT_FALSE(dup);
  EXPECT_EQ(dup.error().code, SQLITE_CONSTRAINT_UNIQUE);
}

TEST_F(StoreTest, TransactionsRollBackUnlessCommitted) {
  Db db = Open();
  ASSERT_TRUE(db.Exec("CREATE TABLE t (x);"));
  {
    auto txn = Txn::Begin(db);
    ASSERT_TRUE(txn);
    ASSERT_TRUE(db.Exec("INSERT INTO t VALUES (1);"));
    Txn moved(std::move(*txn));
  }  // rolled back
  {
    auto txn = Txn::Begin(db);
    ASSERT_TRUE(db.Exec("INSERT INTO t VALUES (2);"));
    ASSERT_TRUE(txn->Commit());
  }
  auto s = db.Prepare("SELECT group_concat(x) FROM t");
  ASSERT_TRUE(s->Step().value());
  EXPECT_EQ(s->Text(0), "2");

  // A failed COMMIT rolls back, so the connection isn't left inside a
  // transaction that every later BEGIN trips over.
  {
    auto txn = Txn::Begin(db);
    ASSERT_TRUE(db.Exec("INSERT INTO t VALUES (3);"));
    Db::FailAfter(1);
    EXPECT_FALSE(txn->Commit());
    Db::FailAfter(0);
  }
  auto again = Txn::Begin(db);
  ASSERT_TRUE(again) << "still inside the failed transaction";
  ASSERT_TRUE(again->Commit());
  auto after = db.Prepare("SELECT group_concat(x) FROM t");
  ASSERT_TRUE(after->Step().value());
  EXPECT_EQ(after->Text(0), "2");

  // A transaction can't begin inside another.
  auto outer = Txn::Begin(db);
  EXPECT_FALSE(Txn::Begin(db));
}

// ---- migrations ---------------------------------------------------------------

TEST_F(StoreTest, MigrationsRunOnceAndRefuseNewerSchemas) {
  Db db = Open();
  EXPECT_EQ(st::SchemaVersion(db).value(), static_cast<int>(st::Migrations().size()));
  EXPECT_TRUE(st::Migrate(db));  // nothing to do

  ASSERT_TRUE(db.Exec("PRAGMA user_version = 99;"));
  auto newer = st::Migrate(db);
  ASSERT_FALSE(newer);
  EXPECT_NE(newer.error().message.find("newer Micnote"), std::string::npos);
}

TEST_F(StoreTest, ABrokenMigrationIsRolledBackAndNamed) {
  auto db = Db::Open(path(), Key(1));
  const std::array<Migration, 2> ms{
      Migration{.name = "0001_ok.sql", .sql = "CREATE TABLE a (x);"},
      Migration{.name = "0002_bad.sql", .sql = "CREATE TABLE b (x); NOT SQL;"}};
  auto r = st::Migrate(*db, ms);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("0002_bad.sql"), std::string::npos);
  EXPECT_EQ(st::SchemaVersion(*db).value(), 1);
  EXPECT_FALSE(db->Prepare("SELECT * FROM b")) << "0002 left a table behind";

  Sweep([&] { return std::move(Db::Open(dir_path() / "sweep.db", Key(2)).value()); },
        [&](Db &d) {
          return d.Exec("PRAGMA user_version = 0; DROP TABLE IF EXISTS a;") &&
                 st::Migrate(d, std::span(ms).first(1)).has_value();
        });
}

// ---- devices and clock ----------------------------------------------------------

TEST_F(StoreTest, Devices) {
  Db db = Open();
  Device d;
  d.device_id = DeviceId{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  d.name = "desk puck";
  d.host_slot = 2;
  d.paired_at = 100;
  const auto id = st::UpsertDevice(db, d).value();

  d.name = "renamed";
  EXPECT_EQ(st::UpsertDevice(db, d).value(), id) << "same device_id, same row";
  ASSERT_TRUE(st::TouchDevice(db, id, 500, "0.2.0"));

  const Device found = Present(st::FindDevice(db, d.device_id).value());
  EXPECT_EQ(found.name, "renamed");
  EXPECT_EQ(found.host_slot, 2);
  EXPECT_EQ(found.last_seen, 500);
  EXPECT_EQ(found.fw_version, "0.2.0");
  EXPECT_FALSE(st::FindDevice(db, DeviceId{}).value());
  EXPECT_EQ(st::ListDevices(db).value().size(), 1U);
}

TEST_F(StoreTest, ClockObservations) {
  Db db = Open();
  const auto dev = AddDevice(db);
  ASSERT_TRUE(st::AddClockObservation(
      db, dev, {.boot_count = 3, .uptime_ms = 9000, .unix_ms = 5, .observed_at = 1}));
  ASSERT_TRUE(st::AddClockObservation(
      db, dev, {.boot_count = 3, .uptime_ms = 2000, .unix_ms = 4, .observed_at = 1}));
  ASSERT_TRUE(st::AddClockObservation(
      db, dev, {.boot_count = 4, .uptime_ms = 1, .unix_ms = 9, .observed_at = 1}));
  const auto obs = st::ClockObservations(db, dev, 3).value();
  ASSERT_EQ(obs.size(), 2U);
  EXPECT_EQ(obs[0].uptime_ms, 2000);
  EXPECT_EQ(obs[1].unix_ms, 5);
}

// ---- recordings ---------------------------------------------------------------

TEST_F(StoreTest, RecordingsRoundTrip) {
  Db db = Open();
  const auto dev = AddDevice(db);
  const auto id = st::InsertRecording(db, Recording(dev, 7)).value();
  ASSERT_TRUE(st::InsertRecording(db, Recording(dev, 8)));

  StoredRecording r = Present(st::GetRecording(db, id).value());
  EXPECT_EQ(r.rec_id, 7);
  EXPECT_EQ(r.mnr_path, "rec/7.mnr");
  EXPECT_EQ(r.state, RecordingState::kSynced);
  EXPECT_EQ(r.time_confidence, TimeConfidence::kUnknown);
  EXPECT_EQ(r.start_unix_ms_fixed, std::nullopt);
  ASSERT_EQ(r.markers.size(), 2U);
  EXPECT_EQ(r.markers[1].offset_ms, 3000);

  ASSERT_TRUE(st::SetRecordingState(db, id, RecordingState::kTranscribed));
  ASSERT_TRUE(st::SetRecordingTime(db, id, 1763800000000, TimeConfidence::kRepaired));
  r = Present(st::GetRecording(db, id).value());
  EXPECT_EQ(r.state, RecordingState::kTranscribed);
  EXPECT_EQ(r.time_confidence, TimeConfidence::kRepaired);
  EXPECT_EQ(r.start_unix_ms_fixed, 1763800000000);

  const auto all = st::ListRecordings(db).value();
  ASSERT_EQ(all.size(), 2U);
  EXPECT_EQ(all[0].rec_id, 8) << "newest first";
  EXPECT_EQ(all[0].markers.size(), 2U);
  EXPECT_EQ(st::ListRecordings(db, 1).value().size(), 1U);

  // The same puck recording isn't stored twice.
  auto dup = st::InsertRecording(db, Recording(dev, 7));
  ASSERT_FALSE(dup);
  EXPECT_EQ(dup.error().code, SQLITE_CONSTRAINT_UNIQUE);

  EXPECT_TRUE(st::DeleteRecording(db, id).value());
  EXPECT_FALSE(st::DeleteRecording(db, id).value());
  EXPECT_FALSE(st::GetRecording(db, id).value());
  auto markers = db.Prepare("SELECT count(*) FROM markers");
  ASSERT_TRUE(markers->Step().value());
  EXPECT_EQ(markers->Int(0), 2) << "the deleted recording's markers went with it";
}

TEST_F(StoreTest, StateNames) {
  EXPECT_STREQ(st::ToString(RecordingState::kError), "error");
  EXPECT_STREQ(st::ToString(RecordingState::kPlanned), "planned");
  EXPECT_STREQ(st::ToString(TimeConfidence::kRtc), "rtc");
}

// ---- transcripts and search -----------------------------------------------------

TEST_F(StoreTest, TranscriptsAndSearch) {
  Db db = Open();
  const auto dev = AddDevice(db);
  const auto rec = st::InsertRecording(db, Recording(dev, 1)).value();
  const auto other = st::InsertRecording(db, Recording(dev, 2)).value();
  Transcript t{.id = 0,
               .recording = rec,
               .model = "small.en",
               .backend = "cpu",
               .language = "en",
               .created_at = 5,
               .rtf = 0.25,
               .segments = {Segment{.start_ms = 0,
                                    .end_ms = 1500,
                                    .text = "email Jeff about the budget",
                                    .avg_logprob = -0.2,
                                    .no_speech_prob = 0.01},
                            Segment{.start_ms = 1500,
                                    .end_ms = 4000,
                                    .text = "and review the pull request",
                                    .avg_logprob = -0.3,
                                    .no_speech_prob = 0.02}}};
  ASSERT_TRUE(st::SaveTranscript(db, t));
  t.recording = other;
  t.segments = {Segment{.start_ms = 0,
                        .end_ms = 900,
                        .text = "budget meeting notes",
                        .avg_logprob = 0,
                        .no_speech_prob = 0}};
  ASSERT_TRUE(st::SaveTranscript(db, t));

  const Transcript got = Present(st::GetTranscript(db, rec).value());
  EXPECT_EQ(got.model, "small.en");
  EXPECT_DOUBLE_EQ(got.rtf, 0.25);
  ASSERT_EQ(got.segments.size(), 2U);
  EXPECT_EQ(got.segments[1].text, "and review the pull request");
  EXPECT_FALSE(st::GetTranscript(db, 999).value());

  auto hits = st::Search(db, "budget").value();
  EXPECT_EQ(hits.size(), 2U);
  hits = st::Search(db, "pull request").value();
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(hits[0].recording, rec);
  EXPECT_EQ(hits[0].start_ms, 1500);
  EXPECT_FALSE(st::Search(db, "\"unbalanced"));

  // Re-transcribing replaces the old segments, in the index too.
  t.recording = rec;
  t.model = "large-v3-turbo";
  t.segments = {Segment{.start_ms = 0,
                        .end_ms = 100,
                        .text = "something else",
                        .avg_logprob = 0,
                        .no_speech_prob = 0}};
  ASSERT_TRUE(st::SaveTranscript(db, t));
  EXPECT_EQ(Present(st::GetTranscript(db, rec).value()).model, "large-v3-turbo");
  EXPECT_EQ(st::Search(db, "pull").value().size(), 0U);
  EXPECT_EQ(st::Search(db, "else").value().size(), 1U);

  // Deleting the recording takes its transcript and index rows along.
  ASSERT_TRUE(st::DeleteRecording(db, other).value());
  EXPECT_EQ(st::Search(db, "budget").value().size(), 0U);
}

TEST_F(StoreTest, Jobs) {
  Db db = Open();
  EXPECT_FALSE(st::ClaimJob(db, "sync", 10).value());
  EXPECT_FALSE(st::NextDue(db, "sync").value());
  EXPECT_FALSE(st::GetJob(db, 1).value());
  const auto a = st::EnqueueJob(db, "sync", std::nullopt, R"({"usb":"x"})", 10).value();
  const auto b = st::EnqueueJob(db, "sync", 7, "{}", 11).value();
  EXPECT_EQ(st::NextDue(db, "sync").value(), 0);
  // Oldest first, one attempt counted.
  const auto claimed = Present(st::ClaimJob(db, "sync", 12).value());
  EXPECT_EQ(claimed.id, a);
  EXPECT_EQ(claimed.state, st::JobState::kRunning);
  EXPECT_EQ(claimed.attempts, 1);
  EXPECT_EQ(claimed.params_json, R"({"usb":"x"})");
  EXPECT_EQ(claimed.updated_at, 12);
  // Not due yet: not claimed.
  ASSERT_TRUE(st::RetryJob(db, a, 100, "away", 13));
  EXPECT_EQ(Present(st::ClaimJob(db, "sync", 50).value()).id, b);
  EXPECT_FALSE(st::ClaimJob(db, "sync", 50).value());
  EXPECT_EQ(st::NextDue(db, "sync").value(), 100);
  EXPECT_EQ(Present(st::GetJob(db, a).value()).last_error, "away");
  // A shutdown puts b back, its attempt not counted.
  ASSERT_TRUE(st::RequeueStoppedJob(db, b, 51));
  EXPECT_EQ(Present(st::GetJob(db, b).value()).attempts, 0);
  EXPECT_EQ(Present(st::GetJob(db, b).value()).ref_id, 7);
  // Running jobs back in the queue at startup.
  ASSERT_TRUE(st::ClaimJob(db, "sync", 52).value());
  EXPECT_EQ(st::RequeueRunningJobs(db, 53).value(), 1);
  // Cancel only what's queued.
  EXPECT_TRUE(st::CancelQueuedJob(db, b, 54).value());
  EXPECT_FALSE(st::CancelQueuedJob(db, b, 55).value());
  ASSERT_TRUE(st::KillJob(db, a, "gone", 56));
  const auto list = st::ListJobs(db).value();
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(list[0].id, b);
  EXPECT_EQ(list[0].state, st::JobState::kCancelled);
  EXPECT_EQ(list[1].state, st::JobState::kDead);
  EXPECT_EQ(st::ListJobs(db, 1).value().size(), 1U);
  ASSERT_TRUE(st::FinishJob(db, a, 57));
  ASSERT_TRUE(st::MarkJobCancelled(db, a, 58));
  // The schema refuses a state that isn't one.
  EXPECT_FALSE(db.Exec("UPDATE jobs SET state = 'lost'"));
  EXPECT_FALSE(
      db.Exec("INSERT INTO jobs (kind, state, created_at, updated_at) "
              "VALUES ('x', 'lost', 1, 1)"));
  EXPECT_STREQ(st::ToString(st::JobState::kQueued), "queued");
  EXPECT_STREQ(st::ToString(st::JobState::kCancelled), "cancelled");
}

TEST_F(StoreTest, FindAndUndatedRecordings) {
  Db db = Open();
  AddDevice(db);
  EXPECT_FALSE(st::FindRecordingSha(db, 1, 5).value());
  const auto id = st::InsertRecording(db, Recording(1, 5)).value();
  EXPECT_EQ(st::FindRecordingSha(db, 1, 5).value(), Recording(1, 5).sha256);
  EXPECT_EQ(st::UndatedRecordings(db, 1, 3).value().size(), 1U);
  EXPECT_EQ(st::UndatedRecordings(db, 1, 4).value().size(), 0U);
  ASSERT_TRUE(st::SetRecordingTime(db, id, 5, TimeConfidence::kRepaired));
  EXPECT_EQ(st::UndatedRecordings(db, 1, 3).value().size(), 0U);
}

TEST_F(StoreTest, Settings) {
  Db db = Open();
  EXPECT_FALSE(st::GetSetting(db, "language").value());
  ASSERT_TRUE(st::SetSetting(db, "language", "\"en\""));
  ASSERT_TRUE(st::SetSetting(db, "language", "\"auto\""));
  EXPECT_EQ(st::GetSetting(db, "language").value(), "\"auto\"");
}

// ---- every failure point ----------------------------------------------------------

Transcript SmallTranscript(std::int64_t recording, std::size_t segments) {
  Transcript t;
  t.recording = recording;
  t.model = "m";
  t.backend = "b";
  t.language = "en";
  t.created_at = 1;
  t.rtf = 1;
  t.segments.resize(segments);
  return t;
}

class StoreFaults : public StoreTest {
 protected:
  std::function<Db()> fresh() {
    return [this] {
      static int n = 0;
      auto db = Db::Open(dir_path() / ("f" + std::to_string(n++) + ".db"), Key(3));
      EXPECT_TRUE(st::Migrate(*db));
      AddDevice(*db);
      return std::move(*db);
    };
  }
};

TEST_F(StoreFaults, EveryRepositoryCallPropagatesErrors) {
  const std::vector<std::pair<const char *, std::function<bool(Db &)>>> ops = {
      {"UpsertDevice",
       [](Db &db) {
         return st::UpsertDevice(db, Device{.id = 0,
                                            .device_id = {std::byte{9}},
                                            .name = "x",
                                            .host_slot = std::nullopt,
                                            .paired_at = 1,
                                            .fw_version = "",
                                            .last_seen = std::nullopt})
             .has_value();
       }},
      {"FindDevice",
       [](Db &db) {
         return st::FindDevice(db, DeviceId{std::byte{1}, std::byte{1}, std::byte{1}, std::byte{1},
                                            std::byte{1}, std::byte{1}, std::byte{1}, std::byte{1}})
             .has_value();
       }},
      {"ListDevices", [](Db &db) { return st::ListDevices(db).has_value(); }},
      {"TouchDevice", [](Db &db) { return st::TouchDevice(db, 1, 2, "v").has_value(); }},
      {"AddClockObservation",
       [](Db &db) {
         return st::AddClockObservation(
                    db, 1,
                    ClockObservation{
                        .boot_count = 1, .uptime_ms = 2, .unix_ms = 3, .observed_at = 4})
             .has_value();
       }},
      {"ClockObservations",
       [](Db &db) {
         return st::AddClockObservation(
                    db, 1,
                    ClockObservation{
                        .boot_count = 1, .uptime_ms = 2, .unix_ms = 3, .observed_at = 4}) &&
                st::ClockObservations(db, 1, 1).has_value();
       }},
      {"InsertRecording",
       [](Db &db) { return st::InsertRecording(db, Recording(1, 5)).has_value(); }},
      {"GetRecording",
       [](Db &db) {
         const auto id = st::InsertRecording(db, Recording(1, 5));
         return id && st::GetRecording(db, *id).has_value();
       }},
      {"ListRecordings",
       [](Db &db) {
         return st::InsertRecording(db, Recording(1, 5)) && st::ListRecordings(db).has_value();
       }},
      {"SetRecordingState",
       [](Db &db) { return st::SetRecordingState(db, 1, RecordingState::kError).has_value(); }},
      {"SetRecordingTime",
       [](Db &db) {
         return st::SetRecordingTime(db, 1, std::nullopt, TimeConfidence::kUnknown).has_value();
       }},
      {"DeleteRecording", [](Db &db) { return st::DeleteRecording(db, 1).has_value(); }},
      {"SaveTranscript",
       [](Db &db) {
         const auto r = st::InsertRecording(db, Recording(1, 5));
         return r && st::SaveTranscript(db, SmallTranscript(*r, 2)).has_value();
       }},
      {"GetTranscript",
       [](Db &db) {
         const auto r = st::InsertRecording(db, Recording(1, 5));
         return r && st::SaveTranscript(db, SmallTranscript(*r, 1)) &&
                st::GetTranscript(db, *r).has_value();
       }},
      {"GetTranscript (none)", [](Db &db) { return st::GetTranscript(db, 42).has_value(); }},
      {"Search", [](Db &db) { return st::Search(db, "x").has_value(); }},
      {"GetSetting", [](Db &db) { return st::GetSetting(db, "k").has_value(); }},
      {"SetSetting", [](Db &db) { return st::SetSetting(db, "k", "1").has_value(); }},
      {"SchemaVersion", [](Db &db) { return st::SchemaVersion(db).has_value(); }},
      {"EnqueueJob", [](Db &db) { return st::EnqueueJob(db, "k", 1, "{}", 1).has_value(); }},
      {"ClaimJob",
       [](Db &db) {
         return st::EnqueueJob(db, "k", std::nullopt, "{}", 1) &&
                st::ClaimJob(db, "k", 1).has_value();
       }},
      {"GetJob",
       [](Db &db) {
         const auto id = st::EnqueueJob(db, "k", std::nullopt, "{}", 1);
         return id && st::GetJob(db, *id).has_value();
       }},
      {"ListJobs",
       [](Db &db) {
         return st::EnqueueJob(db, "k", std::nullopt, "{}", 1) && st::ListJobs(db).has_value();
       }},
      {"FinishJob", [](Db &db) { return st::FinishJob(db, 1, 1).has_value(); }},
      {"RetryJob", [](Db &db) { return st::RetryJob(db, 1, 2, "e", 1).has_value(); }},
      {"KillJob", [](Db &db) { return st::KillJob(db, 1, "e", 1).has_value(); }},
      {"CancelQueuedJob", [](Db &db) { return st::CancelQueuedJob(db, 1, 1).has_value(); }},
      {"MarkJobCancelled", [](Db &db) { return st::MarkJobCancelled(db, 1, 1).has_value(); }},
      {"RequeueStoppedJob", [](Db &db) { return st::RequeueStoppedJob(db, 1, 1).has_value(); }},
      {"RequeueRunningJobs", [](Db &db) { return st::RequeueRunningJobs(db, 1).has_value(); }},
      {"NextDue", [](Db &db) { return st::NextDue(db, "k").has_value(); }},
      {"FindRecordingSha",
       [](Db &db) {
         return st::InsertRecording(db, Recording(1, 5)) &&
                st::FindRecordingSha(db, 1, 5).has_value();
       }},
      {"UndatedRecordings",
       [](Db &db) {
         return st::InsertRecording(db, Recording(1, 5)) &&
                st::UndatedRecordings(db, 1, 3).has_value();
       }},
  };
  for (const auto &[name, op] : ops) {
    SCOPED_TRACE(name);
    Sweep(fresh(), op);
  }
}

}  // namespace
