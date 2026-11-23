// The job queue (stage 2 §4.8), the sync job against the fake puck, and the
// transcribe job with a fake engine. Store failures are injected inside
// DbAccess, on the thread that runs the job.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>
#endif

#include "api/context.hpp"
#include "asr/transcribe.hpp"
#include "fake_puck.hpp"
#include "jobs/queue.hpp"
#include "jobs/sync_job.hpp"
#include "jobs/transcribe_job.hpp"
#include "secrets/secrets.hpp"
#include "store/db.hpp"
#include "store/repos.hpp"
#include "store/schema.hpp"
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
namespace st = micnote::store;
namespace jobs = micnote::jobs;
using jobs::JobError;
using jobs::JobQueue;
using jobs::JobResult;
using micnote::test::Borrowed;
using micnote::test::FakePuck;
using nlohmann::json;
using ErrKind = JobError::Kind;

st::DbKey Key() {
  st::DbKey k{};
  k.fill(std::byte{0x5A});
  return k;
}

// A migrated store, the DbAccess the queue and jobs use (with optional
// fault injection on the calling thread), events, and a settable clock.
class Fixture : public ::testing::Test {
 public:
  Fixture() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-jobs-test-" + std::to_string(rd()));
    fs::create_directories(dir_);
    auto db = st::Db::Open(dir_ / "micnote.db", Key());
    EXPECT_TRUE(db);
    EXPECT_TRUE(st::Migrate(*db));
    db_ = std::make_unique<st::Db>(std::move(*db));
    access_ = [this](const std::function<void(st::Db &)> &fn) {
      const std::scoped_lock lock(db_mu_);
      if (fail_next_access_ > 0) {
        st::Db::FailAfter(fail_next_access_);
        fail_next_access_ = 0;
      }
      fn(*db_);
      st::Db::FailAfter(0);
    };
  }
  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
  Fixture(Fixture &&) = delete;
  Fixture &operator=(Fixture &&) = delete;
  ~Fixture() override {
    db_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  void Emit(const std::string &e) {
    const std::scoped_lock lock(ev_mu_);
    events_.push_back(json::parse(e));
    ev_cv_.notify_all();
  }
  std::function<void(const std::string &)> Emitter() {
    return [this](const std::string &e) { Emit(e); };
  }
  std::function<std::int64_t()> Clock() {
    return [this] { return now_.load(); };
  }
  // Waits for an event of `type` (for `job`, if given); fails the test after 10 s.
  json WaitFor(const std::string &type, std::int64_t job = -1) {
    std::unique_lock lock(ev_mu_);
    json found;
    const bool ok = ev_cv_.wait_for(lock, std::chrono::seconds(10), [&] {
      for (const auto &e : events_) {
        if (e["type"] == type && (job < 0 || e.value("job", std::int64_t{-2}) == job)) {
          found = e;
          return true;
        }
      }
      return false;
    });
    EXPECT_TRUE(ok) << "no " << type << " event";
    return found;
  }
  std::size_t Count(const std::string &type) {
    const std::scoped_lock lock(ev_mu_);
    return static_cast<std::size_t>(
        std::ranges::count_if(events_, [&](const json &e) { return e["type"] == type; }));
  }
  void ClearEvents() {
    const std::scoped_lock lock(ev_mu_);
    events_.clear();
  }
  st::Job Job(std::int64_t id) {
    st::Job j;
    access_([&](st::Db &db) { j = st::GetJob(db, id).value().value(); });
    return j;
  }
  // The `n`-th database call inside the next DbAccess fails.
  void FailNextAccess(int n) {
    const std::scoped_lock lock(db_mu_);
    fail_next_access_ = n;
  }

  [[nodiscard]] const fs::path &dir() const { return dir_; }
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): test fixture state
  jobs::DbAccess access_;
  std::atomic<std::int64_t> now_{1'000'000};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

 private:
  fs::path dir_;
  std::mutex db_mu_;
  std::unique_ptr<st::Db> db_;
  int fail_next_access_ = 0;
  std::mutex ev_mu_;
  std::condition_variable ev_cv_;
  std::vector<json> events_;
};

// ---- the queue ------------------------------------------------------------------

using QueueTest = Fixture;

TEST_F(QueueTest, RunsJobsAndReportsProgress) {
  JobQueue q(access_, Emitter(), Clock());
  std::atomic<int> ran{0};
  q.Register("work", [&](jobs::JobContext &ctx) -> JobResult {
    ctx.Progress(50);
    EXPECT_EQ(ctx.job().params_json, R"({"x":1})");
    EXPECT_EQ(ctx.Now(), 1'000'000);
    ++ran;
    return {};
  });
  EXPECT_TRUE(q.Handles("work"));
  EXPECT_FALSE(q.Handles("other"));
  ASSERT_TRUE(q.Start());
  const auto id = q.Submit("work", 7, R"({"x":1})").value();
  WaitFor("job.done", id);
  EXPECT_EQ(WaitFor("job.progress", id)["pct"], 50);
  EXPECT_EQ(ran, 1);
  const st::Job j = Job(id);
  EXPECT_EQ(j.state, st::JobState::kDone);
  EXPECT_EQ(j.attempts, 1);
  EXPECT_EQ(j.ref_id, 7);
}

TEST_F(QueueTest, RetriesWithBackoffThenGivesUp) {
  JobQueue q(access_, Emitter(), Clock());
  std::atomic<int> ran{0};
  q.Register("flaky", [&](jobs::JobContext &) -> JobResult {
    ++ran;
    return std::unexpected(JobError{.kind = ErrKind::kRetryable, .message = "puck away"});
  });
  ASSERT_TRUE(q.Start());
  const auto id = q.Submit("flaky", std::nullopt, "{}").value();
  for (int attempt = 1; attempt <= jobs::kMaxAttempts; ++attempt) {
    const json e = WaitFor("job.failed", id);
    const st::Job j = Job(id);
    EXPECT_EQ(j.attempts, attempt);
    EXPECT_EQ(j.last_error, "puck away");
    if (attempt < jobs::kMaxAttempts) {
      EXPECT_EQ(j.state, st::JobState::kQueued);
      EXPECT_EQ(e["retry_at"], jobs::RetryAt(now_, attempt));
      // Not before its time...
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      EXPECT_EQ(ran, attempt);
      // ...but as soon as it's due.
      ClearEvents();
      now_ = j.not_before;
      q.Poke();
    } else {
      EXPECT_EQ(j.state, st::JobState::kDead);
      EXPECT_EQ(e["retry_at"], nullptr);
    }
  }
  EXPECT_EQ(jobs::RetryAt(0, 1), 2 * 60 * 1000);
  EXPECT_EQ(jobs::RetryAt(0, 3), 8 * 60 * 1000);
}

TEST_F(QueueTest, AFatalErrorIsFinal) {
  JobQueue q(access_, Emitter(), Clock());
  q.Register("bad", [](jobs::JobContext &) -> JobResult {
    return std::unexpected(JobError{.kind = ErrKind::kFatal, .message = "not paired"});
  });
  ASSERT_TRUE(q.Start());
  const auto id = q.Submit("bad", std::nullopt, "{}").value();
  EXPECT_EQ(WaitFor("job.failed", id)["error"], "not paired");
  EXPECT_EQ(Job(id).state, st::JobState::kDead);
}

TEST_F(QueueTest, CancelQueuedAndRunning) {
  JobQueue q(access_, Emitter(), Clock());
  std::atomic<bool> started{false};
  q.Register("long", [&](jobs::JobContext &ctx) -> JobResult {
    started = true;
    while (!ctx.Cancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::unexpected(JobError{.kind = ErrKind::kStopped, .message = "stopped"});
  });
  // Queued (the queue isn't running yet): cancelled in the store.
  const auto queued = q.Submit("long", std::nullopt, "{}").value();
  EXPECT_TRUE(q.Cancel(queued));
  EXPECT_EQ(Job(queued).state, st::JobState::kCancelled);
  EXPECT_FALSE(q.Cancel(queued));
  EXPECT_FALSE(q.Cancel(12345));

  ASSERT_TRUE(q.Start());
  const auto running = q.Submit("long", std::nullopt, "{}").value();
  while (!started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(q.Cancel(running));
  WaitFor("job.cancelled", running);
  EXPECT_EQ(Job(running).state, st::JobState::kCancelled);
}

TEST_F(QueueTest, StoppingPausesARunningJobForNextTime) {
  std::atomic<bool> started{false};
  const auto hold = [&](jobs::JobContext &ctx) -> JobResult {
    started = true;
    while (!ctx.Cancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::unexpected(JobError{.kind = ErrKind::kStopped, .message = "stopped"});
  };
  std::int64_t id = 0;
  {
    JobQueue q(access_, Emitter(), Clock());
    q.Register("long", hold);
    ASSERT_TRUE(q.Start());
    id = q.Submit("long", std::nullopt, "{}").value();
    while (!started) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }  // stopped
  EXPECT_EQ(WaitFor("job.paused", id)["kind"], "long");
  st::Job j = Job(id);
  EXPECT_EQ(j.state, st::JobState::kQueued);
  EXPECT_EQ(j.attempts, 0) << "a paused attempt doesn't count";

  // The next queue picks it up.
  JobQueue again(access_, Emitter(), Clock());
  again.Register("long", [](jobs::JobContext &) -> JobResult { return {}; });
  ASSERT_TRUE(again.Start());
  WaitFor("job.done", id);
}

TEST_F(QueueTest, APausedKindStartsNothingUntilResumed) {
  JobQueue q(access_, Emitter(), Clock());
  std::atomic<int> ran{0};
  q.Register("transcribe", [&](jobs::JobContext &) -> JobResult {
    ++ran;
    return {};
  });
  q.SetPaused("transcribe", true);
  ASSERT_TRUE(q.Start());
  const auto id = q.Submit("transcribe", 1, "{}").value();
  q.Poke();  // a poke alone doesn't unpause
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(ran, 0);
  EXPECT_EQ(Job(id).state, st::JobState::kQueued);
  q.SetPaused("transcribe", false);
  WaitFor("job.done", id);
  EXPECT_EQ(ran, 1);
  // Stopping while the worker waits paused.
  q.SetPaused("transcribe", true);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  q.Stop();
}

TEST_F(QueueTest, ACrashedRunIsRequeuedAtStart) {
  std::int64_t id = 0;
  access_([&](st::Db &d) {
    id = st::EnqueueJob(d, "work", std::nullopt, "{}", 1).value();
    ASSERT_TRUE(st::ClaimJob(d, "work", 1).value().has_value());  // now `running`
  });
  JobQueue q(access_, Emitter(), Clock());
  q.Register("work", [](jobs::JobContext &) -> JobResult { return {}; });
  ASSERT_TRUE(q.Start());
  WaitFor("job.done", id);
}

TEST_F(QueueTest, StoreErrors) {
  JobQueue q(access_, Emitter(), Clock());
  q.Register("work", [](jobs::JobContext &) -> JobResult { return {}; });

  // Start's requeue failing is reported (and the workers run anyway).
  FailNextAccess(1);
  EXPECT_FALSE(q.Start());
  // Submit failing.
  FailNextAccess(1);
  EXPECT_FALSE(q.Submit("work", std::nullopt, "{}"));
  // Cancel failing: reported as "not cancelled".
  FailNextAccess(1);
  EXPECT_FALSE(q.Cancel(1));

  // A claim failing: the worker backs off and tries again.
  const auto id = q.Submit("work", std::nullopt, "{}");
  ASSERT_TRUE(id);
  WaitFor("job.done", *id);

  // Settling failing: reported; the job stays running for the next Start.
  ClearEvents();
  std::int64_t id2 = 0;
  {
    JobQueue q2(access_, Emitter(), Clock());
    q2.Register("slow", [&](jobs::JobContext &) -> JobResult {
      FailNextAccess(1);  // the settle that follows
      return {};
    });
    ASSERT_TRUE(q2.Start());
    id2 = q2.Submit("slow", std::nullopt, "{}").value();
    WaitFor("job.store_error", id2);
  }
  EXPECT_EQ(Job(id2).state, st::JobState::kRunning);
}

TEST_F(QueueTest, AClaimThatFailsIsRetried) {
  JobQueue q(access_, Emitter(), Clock());
  q.Register("work", [](jobs::JobContext &) -> JobResult { return {}; });
  std::int64_t id = 0;
  access_([&](st::Db &d) { id = st::EnqueueJob(d, "work", std::nullopt, "{}", 1).value(); });
  FailNextAccess(1);  // Start's requeue: reported
  EXPECT_FALSE(q.Start());
  FailNextAccess(1);  // the first claim
  q.Poke();
  WaitFor("job.done", id);
}

// ---- the sync job -------------------------------------------------------------------

constexpr micnote::crypto::LinkKey kLink = [] {
  micnote::crypto::LinkKey k{};
  for (std::size_t i = 0; i < k.size(); ++i) {
    k.at(i) = static_cast<std::byte>(0x40 + i);
  }
  return k;
}();

class SyncTest : public Fixture {
 public:
  SyncTest() : secrets_(dir() / "secrets") {
    micnote::test::LoadVectors(puck_);
    puck_.strict = true;
    const auto host = micnote::test::KeyNamed("host_a");
    std::array<std::byte, 8> id{};
    std::ranges::copy(host.host_id(), id.begin());
    puck_.hosts[id] = {.link_key = kLink, .name = "host_a"};
    StoreHostKey("host_a");
    EXPECT_TRUE(secrets_.Set(micnote::secrets::LinkKeyName("0102030405060708"), kLink));
    deps_ =
        jobs::SyncDeps{.data_dir = dir() / "data",
                       .secrets = &secrets_,
                       .open = [this](const std::string &port)
                           -> std::expected<std::unique_ptr<micnote::transport::Transport>,
                                            micnote::transport::TransportError> {
                         if (port != "fake") {
                           return std::unexpected(micnote::transport::TransportError::kIo);
                         }
                         puck_.Reconnect();
                         return std::make_unique<Borrowed>(puck_);
                       },
                       .timeout = std::chrono::milliseconds(20),
                       .on_recording = [this](std::int64_t r) { new_recordings_.push_back(r); }};
  }

 protected:
  void StoreHostKey(const std::string &name) {
    const std::string hex = micnote::test::Vectors()["keys"][name]["priv"].get<std::string>();
    std::vector<std::byte> priv;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      priv.push_back(static_cast<std::byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    EXPECT_TRUE(secrets_.Set(micnote::secrets::kHostKey, priv));
  }

  JobResult Run(const std::string &params = R"({"usb":"fake"})", bool cancelled = false) {
    st::Job job;
    job.id = 1;
    job.kind = "sync";
    job.params_json = params;
    const std::atomic<bool> flag{cancelled};
    jobs::JobContext ctx(job, access_, Emitter(), flag, Clock());
    return jobs::RunSync(ctx, deps_);
  }

  std::vector<st::StoredRecording> Stored() {
    std::vector<st::StoredRecording> out;
    access_([&](st::Db &d) { out = st::ListRecordings(d).value(); });
    return out;
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): test fixture state
  FakePuck puck_;
  micnote::secrets::FileSecrets secrets_;
  jobs::SyncDeps deps_;
  std::vector<std::int64_t> new_recordings_;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

TEST_F(SyncTest, StoresAcksAndChains) {
  ASSERT_TRUE(Run()) << Run().error().message;
  const auto stored = Stored();
  // The fallback-key recording isn't for this host: left on the puck.
  ASSERT_EQ(stored.size(), 2U);
  EXPECT_EQ(puck_.recordings.size(), 1U);
  EXPECT_EQ(new_recordings_.size(), 2U);
  EXPECT_EQ(Count("recording.new"), 2U);
  for (const auto &r : stored) {
    EXPECT_TRUE(fs::exists(deps_.data_dir / r.mnr_path)) << r.mnr_path;
    EXPECT_EQ(r.sha256.size(), 32U);
    EXPECT_EQ(r.state, st::RecordingState::kSynced);
    EXPECT_EQ(r.time_confidence, st::TimeConfidence::kRtc);
  }
  EXPECT_EQ(puck_.last_set_time, 1'000'000U);
  // The device and a clock observation.
  std::vector<st::Device> devices;
  access_([&](st::Db &d) { devices = st::ListDevices(d).value(); });
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices[0].last_seen, 1'000'000);
  EXPECT_EQ(devices[0].fw_version, "0.0.0");
  std::vector<st::ClockObservation> obs;
  access_([&](st::Db &d) { obs = st::ClockObservations(d, devices[0].id, 0).value(); });
  EXPECT_EQ(obs.size(), 1U);

  // Again: nothing new, no duplicates.
  ASSERT_TRUE(Run());
  EXPECT_EQ(Stored().size(), 2U);
  EXPECT_EQ(new_recordings_.size(), 2U);
}

TEST_F(SyncTest, AStoredButUnackedRecordingIsAckedNotStoredAgain) {
  ASSERT_TRUE(Run());
  micnote::test::LoadVectors(puck_);  // as if the acks were lost
  ASSERT_EQ(puck_.recordings.size(), 3U);
  ASSERT_TRUE(Run());
  EXPECT_EQ(Stored().size(), 2U);
  EXPECT_EQ(puck_.recordings.size(), 1U);
  EXPECT_EQ(new_recordings_.size(), 2U);
}

// A recording of the fake puck's boot 0 with no time, stored earlier.
std::int64_t SeedUndated(st::Db &d) {
  st::Device dev;
  dev.device_id = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                   std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  dev.paired_at = 1;
  const auto device = st::UpsertDevice(d, dev).value();
  st::StoredRecording r;
  r.device = device;
  r.rec_id = 99;
  r.start_boot = 0;
  r.start_uptime_ms = 0;
  r.mnr_path = "x.mnr";
  r.created_at = 1;
  return st::InsertRecording(d, r).value();
}

TEST_F(SyncTest, RepairsUndatedRecordingsOfTheSameBoot) {
  std::int64_t rec = 0;
  access_([&](st::Db &d) { rec = SeedUndated(d); });
  ASSERT_TRUE(Run());
  st::StoredRecording r;
  access_([&](st::Db &d) { r = st::GetRecording(d, rec).value().value(); });
  EXPECT_EQ(r.time_confidence, st::TimeConfidence::kRepaired);
  EXPECT_EQ(r.start_unix_ms_fixed, 1'000'000);
}

TEST_F(SyncTest, SkipsARecordingStillInProgress) {
  puck_.incomplete_id = 0x0000000100000001ULL;
  ASSERT_TRUE(Run());
  EXPECT_EQ(Stored().size(), 1U);
}

TEST_F(SyncTest, Failures) {
  const auto kind = [&](const std::string &params = R"({"usb":"fake"})") {
    const auto r = Run(params);
    EXPECT_FALSE(r);
    return r ? ErrKind::kStopped : r.error().kind;
  };
  EXPECT_EQ(kind("{}"), ErrKind::kFatal);
  EXPECT_EQ(kind("[1]"), ErrKind::kFatal);
  EXPECT_EQ(kind(R"({"usb":1})"), ErrKind::kFatal);
  EXPECT_EQ(kind(R"({"usb":"nope"})"), ErrKind::kRetryable);
  for (const auto tag :
       {micnote_v1_Frame_hello_tag, micnote_v1_Frame_auth_tag, micnote_v1_Frame_set_time_tag,
        micnote_v1_Frame_get_status_tag, micnote_v1_Frame_list_recordings_tag,
        micnote_v1_Frame_fetch_tag, micnote_v1_Frame_ack_tag}) {
    puck_.error_on = tag;  // Error{BUSY}: try again later
    EXPECT_EQ(kind(), ErrKind::kRetryable) << tag;
  }
  puck_.error_on.reset();

  // The puck goes quiet: a timeout, worth trying again.
  puck_.override_next = [](const micnote_v1_Frame &) { return std::vector<micnote_v1_Frame>{}; };
  EXPECT_EQ(kind(), ErrKind::kRetryable);

  // Cancelled before the first recording.
  EXPECT_EQ(Run(R"({"usb":"fake"})", true).error().kind, ErrKind::kStopped);
}

TEST_F(SyncTest, AckOfAStoredRecordingFails) {
  ASSERT_TRUE(Run());
  micnote::test::LoadVectors(puck_);
  puck_.error_on = micnote_v1_Frame_ack_tag;
  EXPECT_EQ(Run().error().kind, ErrKind::kRetryable);
}

TEST_F(SyncTest, PairingProblemsAreFinal) {
  // No link key for this puck.
  EXPECT_TRUE(
      secrets_.Set(micnote::secrets::LinkKeyName("0102030405060708"), std::vector<std::byte>(3)));
  EXPECT_EQ(Run().error().kind, ErrKind::kFatal);
  // Another link key: the puck's proof doesn't verify.
  auto other = kLink;
  other[0] ^= std::byte{1};
  EXPECT_TRUE(secrets_.Set(micnote::secrets::LinkKeyName("0102030405060708"), other));
  const auto wrong = Run();
  EXPECT_EQ(wrong.error().kind, ErrKind::kFatal);
  EXPECT_NE(wrong.error().message.find("didn't prove"), std::string::npos);
  // A host the puck doesn't know: UNPAIRED.
  StoreHostKey("host_b");
  EXPECT_EQ(Run().error().kind, ErrKind::kFatal);
  // No host key at all.
  EXPECT_TRUE(secrets_.Set(micnote::secrets::kHostKey, std::vector<std::byte>(5)));
  EXPECT_EQ(Run().error().kind, ErrKind::kFatal);
}

TEST_F(SyncTest, CantWriteTheFile) {
  fs::create_directories(deps_.data_dir);
  std::ofstream(deps_.data_dir / "rec") << "a file where the directory goes";
  EXPECT_EQ(Run().error().kind, ErrKind::kRetryable);
  EXPECT_EQ(Stored().size(), 0U);
}

TEST_F(SyncTest, CantRenameIntoPlace) {
  // Where each file goes, from a first sync...
  ASSERT_TRUE(Run());
  const auto stored = Stored();
  access_([&](st::Db &d) { ASSERT_TRUE(d.Exec("DELETE FROM recordings")); });
  // ...is now a directory that isn't empty.
  for (const auto &r : stored) {
    const fs::path p = deps_.data_dir / r.mnr_path;
    fs::remove(p);
    fs::create_directories(p / "in-the-way");
  }
  micnote::test::LoadVectors(puck_);
  EXPECT_EQ(Run().error().kind, ErrKind::kRetryable);
  EXPECT_EQ(Stored().size(), 0U);
}

TEST_F(SyncTest, StoreFailuresAreRetryable) {
  // Fail the n-th database call of each access in turn, until a run gets
  // through: every store step reports a retryable error.
  for (int access = 0; access < 8; ++access) {
    for (int n = 1; n < 12; ++n) {
      std::atomic<int> seen{0};
      const jobs::DbAccess inner = access_;
      deps_.on_recording = {};
      const jobs::DbAccess failing = [&, inner](const std::function<void(st::Db &)> &fn) {
        if (seen++ == access) {
          FailNextAccess(n);
        }
        inner(fn);
      };
      st::Job job;
      job.id = 1;
      job.kind = "sync";
      job.params_json = R"({"usb":"fake"})";
      const std::atomic<bool> flag{false};
      jobs::JobContext ctx(job, failing, Emitter(), flag, Clock());
      // Every step runs each time: nothing stored yet, and one recording
      // waiting for a time.
      access_([&](st::Db &d) {
        ASSERT_TRUE(d.Exec("DELETE FROM recordings"));
        SeedUndated(d);
      });
      micnote::test::LoadVectors(puck_);
      const auto r = jobs::RunSync(ctx, deps_);
      if (!r) {
        EXPECT_EQ(r.error().kind, ErrKind::kRetryable) << access << "/" << n;
      }
    }
  }
}

// ---- the transcribe job ------------------------------------------------------------

class FakeEngine final : public micnote::asr::AsrEngine {
 public:
  std::expected<std::vector<micnote::asr::AsrSegment>, micnote::asr::AsrError> Run(
      std::span<const float> pcm, const micnote::asr::AsrOptions & /*options*/) override {
    if (fail) {
      return std::unexpected(micnote::asr::AsrError::kFailed);
    }
    return std::vector<micnote::asr::AsrSegment>{
        {.start_ms = 0,
         .end_ms = static_cast<std::int64_t>(pcm.size() / 16),
         .text = "email Jeff about the budget",
         .avg_logprob = -0.2,
         .no_speech_prob = 0.1}};
  }
  [[nodiscard]] std::string Model() const override { return "fake-model"; }
  [[nodiscard]] std::string Backend() const override { return "test"; }
  bool fail = false;
};

class TranscribeTest : public SyncTest {
 public:
  TranscribeTest() {
    EXPECT_TRUE(SyncTest::Run());
    rec_ = Stored().at(0).id;
    tdeps_ = jobs::TranscribeDeps{
        .data_dir = deps_.data_dir,
        .secrets = &secrets_,
        .engine = [this]
        -> std::expected<std::shared_ptr<micnote::asr::AsrEngine>, micnote::asr::AsrError> {
          if (no_engine_) {
            return std::unexpected(micnote::asr::AsrError::kModel);
          }
          return engine_;
        },
        .options = {}};
  }

 protected:
  JobResult Transcribe(std::optional<std::int64_t> ref, bool cancelled = false) {
    st::Job job;
    job.id = 2;
    job.kind = "transcribe";
    job.ref_id = ref;
    const std::atomic<bool> flag{cancelled};
    jobs::JobContext ctx(job, access_, Emitter(), flag, Clock());
    return jobs::RunTranscribe(ctx, tdeps_);
  }
  st::RecordingState State() {
    st::RecordingState s{};
    access_([&](st::Db &d) { s = st::GetRecording(d, rec_).value().value().state; });
    return s;
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): test fixture state
  std::int64_t rec_ = 0;
  std::shared_ptr<FakeEngine> engine_ = std::make_shared<FakeEngine>();
  bool no_engine_ = false;
  jobs::TranscribeDeps tdeps_;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

TEST_F(TranscribeTest, StoresTheTranscript) {
  ASSERT_TRUE(Transcribe(rec_));
  EXPECT_EQ(State(), st::RecordingState::kTranscribed);
  st::Transcript t;
  access_([&](st::Db &d) { t = st::GetTranscript(d, rec_).value().value(); });
  EXPECT_EQ(t.model, "fake-model");
  EXPECT_EQ(t.backend, "test");
  ASSERT_EQ(t.segments.size(), 1U);
  EXPECT_EQ(t.segments[0].text, "email Jeff about the budget");
  EXPECT_GT(t.segments[0].end_ms, 1000);
  EXPECT_EQ(WaitFor("transcript.ready")["recording"], rec_);
  EXPECT_GE(Count("job.progress"), 1U);
}

TEST_F(TranscribeTest, Failures) {
  EXPECT_EQ(Transcribe(std::nullopt).error().kind, ErrKind::kFatal);
  EXPECT_EQ(Transcribe(424242).error().kind, ErrKind::kFatal);

  // No engine yet (the model isn't downloaded): retry later, recording fine.
  no_engine_ = true;
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kRetryable);
  EXPECT_EQ(State(), st::RecordingState::kSynced);
  no_engine_ = false;

  // Cancelled: back to synced.
  EXPECT_EQ(Transcribe(rec_, true).error().kind, ErrKind::kStopped);
  EXPECT_EQ(State(), st::RecordingState::kSynced);

  // The engine fails: the recording is marked.
  engine_->fail = true;
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kFatal);
  EXPECT_EQ(State(), st::RecordingState::kError);
  engine_->fail = false;
}

TEST_F(TranscribeTest, FileAndKeyProblems) {
  st::StoredRecording r;
  access_([&](st::Db &d) { r = st::GetRecording(d, rec_).value().value(); });
  // Another host's key can't decrypt it.
  StoreHostKey("host_b");
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kFatal);
  EXPECT_EQ(State(), st::RecordingState::kError);
  StoreHostKey("host_a");
  // A host key that isn't one, and none at all.
  EXPECT_TRUE(secrets_.Set(micnote::secrets::kHostKey, std::vector<std::byte>(1)));
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kFatal);
  fs::remove(dir() / "secrets" / micnote::secrets::kHostKey);
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kFatal);
  StoreHostKey("host_a");
  // The file is gone.
  fs::remove(deps_.data_dir / r.mnr_path);
  EXPECT_EQ(Transcribe(rec_).error().kind, ErrKind::kFatal);
}

TEST_F(TranscribeTest, StoreFailuresAreRetryable) {
  for (int access = 0; access < 2; ++access) {
    // The first access (load) and the last (save); the save is the 3rd.
    const int which = access == 0 ? 0 : 1;
    for (int n = 1; n < 10; ++n) {
      std::atomic<int> seen{0};
      const jobs::DbAccess inner = access_;
      const jobs::DbAccess failing = [&, inner](const std::function<void(st::Db &)> &fn) {
        if (seen++ == (which == 0 ? 0 : 1)) {
          FailNextAccess(n);
        }
        inner(fn);
      };
      st::Job job;
      job.id = 3;
      job.kind = "transcribe";
      job.ref_id = rec_;
      const std::atomic<bool> flag{false};
      jobs::JobContext ctx(job, failing, Emitter(), flag, Clock());
      const auto r = jobs::RunTranscribe(ctx, tdeps_);
      if (!r) {
        EXPECT_EQ(r.error().kind, ErrKind::kRetryable) << access << "/" << n;
      }
    }
  }
}

// ---- through the Context: submit, chain, cancel, jobs.* ------------------------------

class PipelineTest : public TranscribeTest {
 public:
  PipelineTest() { ClearEvents(); }
  PipelineTest(const PipelineTest &) = delete;
  PipelineTest &operator=(const PipelineTest &) = delete;
  PipelineTest(PipelineTest &&) = delete;
  PipelineTest &operator=(PipelineTest &&) = delete;
  ~PipelineTest() override { ctx_.reset(); }

 protected:
  // A context over dir()/ctx whose secrets are the ones the sync fixture
  // stored (host key, link key), with the fake puck and fake engine.
  void Open(bool with_engine) {
    micnote::api::ContextOptions o;
    o.open = deps_.open;
    if (with_engine) {
      o.engine = tdeps_.engine;
    }
    o.now = Clock();
    o.link_timeout = std::chrono::milliseconds(20);
    auto ctx = micnote::api::Context::Open(
        dir() / "ctx", std::make_unique<micnote::secrets::FileSecrets>(dir() / "secrets"),
        &PipelineTest::OnEvent, this, std::move(o));
    ASSERT_TRUE(ctx) << ctx.error().message;
    ctx_ = std::move(*ctx);
    micnote::test::LoadVectors(puck_);
  }
  json Call(const std::string &method, const std::string &params) {
    const auto r = ctx_->Call(method, params);
    json body = json::parse(r.json);
    body["_status"] = r.status;
    return body;
  }
  static void OnEvent(void *ud, const char *event_json) {
    static_cast<PipelineTest *>(ud)->Emit(event_json);
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): test fixture state
  std::unique_ptr<micnote::api::Context> ctx_;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

TEST_F(PipelineTest, SyncChainsIntoTranscription) {
  Open(true);
  const auto sub = ctx_->Submit("sync.run@1", R"({"usb":"fake"})");
  ASSERT_EQ(sub.status, MN_OK) << sub.error_json;
  ASSERT_GT(sub.job, 0U);
  WaitFor("job.done", static_cast<std::int64_t>(sub.job));
  // Both new recordings get transcribed.
  for (int i = 0; i < 2; ++i) {
    WaitFor("transcript.ready");
    std::this_thread::yield();
  }
  const json rec_list = Call("recordings.list@1", "{}");
  const json jobs_list = Call("jobs.list@1", "{}");
  ASSERT_EQ(jobs_list["_status"], MN_OK);
  // One sync and two transcriptions.
  EXPECT_EQ(jobs_list["jobs"].size(), 3U);
  EXPECT_EQ(jobs_list["jobs"].back()["kind"], "sync");
  EXPECT_EQ(jobs_list["jobs"].back()["state"], "done");
  EXPECT_EQ(jobs_list["jobs"].back()["ref_id"], nullptr);
  const json got = Call("jobs.get@1", R"({"id":)" + std::to_string(sub.job) + "}");
  EXPECT_EQ(got["attempts"], 1);
  EXPECT_EQ(Call("jobs.get@1", R"({"id":999})")["_status"], MN_E_NOT_FOUND);
  EXPECT_EQ(Call("jobs.get@1", R"({"id":"x"})")["_status"], MN_E_INVALID);
  static_cast<void>(rec_list);

  // Transcribing again on request.
  const auto again =
      ctx_->Submit("transcribe.run@1", R"({"recording":)" + std::to_string(rec_) + "}");
  ASSERT_EQ(again.status, MN_OK);
  WaitFor("job.done", static_cast<std::int64_t>(again.job));
}

TEST_F(PipelineTest, WithoutAnEngineRecordingsStaySynced) {
  Open(false);
  const auto sub = ctx_->Submit("sync.run@1", R"({"usb":"fake"})");
  WaitFor("job.done", static_cast<std::int64_t>(sub.job));
  EXPECT_EQ(Count("transcript.ready"), 0U);
  const auto t = ctx_->Submit("transcribe.run@1", R"({"recording":1})");
  EXPECT_EQ(t.status, MN_E_MODEL);
  EXPECT_EQ(t.job, 0U);
  EXPECT_NE(t.error_json.find("no_engine"), std::string::npos);
}

TEST_F(PipelineTest, BadSubmissions) {
  Open(true);
  const auto status = [&](const std::string &method, const std::string &params) {
    return ctx_->Submit(method, params).status;
  };
  EXPECT_EQ(status("sync.run@1", "not json"), MN_E_INVALID);
  EXPECT_EQ(status("sync.run@1", "[]"), MN_E_INVALID);
  EXPECT_EQ(status("sync.run@1", "{}"), MN_E_INVALID);
  EXPECT_EQ(status("sync.run@1", ""), MN_E_INVALID);
  EXPECT_EQ(status("transcribe.run@1", R"({"recording":"x"})"), MN_E_INVALID);
  EXPECT_EQ(status("recordings.list@1", "{}"), MN_E_INVALID);
  EXPECT_EQ(ctx_->Cancel(4242), MN_E_NOT_FOUND);

  // The store failing (on this thread, where Submit and Call run).
  st::Db::FailAfter(1);
  EXPECT_EQ(status("sync.run@1", R"({"usb":"fake"})"), MN_E_IO);
  st::Db::FailAfter(1);
  EXPECT_EQ(Call("jobs.get@1", R"({"id":1})")["_status"], MN_E_IO);
  st::Db::FailAfter(1);
  EXPECT_EQ(Call("jobs.list@1", "{}")["_status"], MN_E_IO);
  st::Db::FailAfter(0);
}

#if !defined(_WIN32)
// The real USB opener: a port that isn't there, and a pty that never
// answers (a puck that's plugged in but not talking).
TEST_F(PipelineTest, TheDefaultOpenerOpensSerialPorts) {
  micnote::api::ContextOptions o;
  o.now = Clock();
  o.link_timeout = std::chrono::milliseconds(20);
  auto ctx = micnote::api::Context::Open(
      dir() / "ctx", std::make_unique<micnote::secrets::FileSecrets>(dir() / "secrets"),
      &PipelineTest::OnEvent, this, std::move(o));
  ASSERT_TRUE(ctx);
  ctx_ = std::move(*ctx);

  const auto missing = ctx_->Submit("sync.run@1", R"({"usb":"/dev/micnote-nowhere"})");
  EXPECT_NE(WaitFor("job.failed", static_cast<std::int64_t>(missing.job))["error"]
                .get<std::string>()
                .find("can't open"),
            std::string::npos);

  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  ASSERT_GE(master, 0);
  ASSERT_EQ(grantpt(master), 0);
  ASSERT_EQ(unlockpt(master), 0);
  const std::string pty = ptsname(master);  // NOLINT(concurrency-mt-unsafe): one thread
  const auto silent = ctx_->Submit("sync.run@1", json{{"usb", pty}}.dump());
  EXPECT_NE(
      WaitFor("job.failed", static_cast<std::int64_t>(silent.job))["error"].get<std::string>().find(
          "Hello"),
      std::string::npos);
  ctx_.reset();
  close(master);
}
#endif

TEST_F(PipelineTest, CancelAndRetry) {
  Open(false);
  // A port that doesn't open: retried later, then cancelled while queued.
  const auto sub = ctx_->Submit("sync.run@1", R"({"usb":"elsewhere"})");
  const json failed = WaitFor("job.failed", static_cast<std::int64_t>(sub.job));
  EXPECT_EQ(failed["retry_at"], jobs::RetryAt(now_, 1));
  EXPECT_EQ(ctx_->Cancel(sub.job), MN_OK);
  WaitFor("job.cancelled", static_cast<std::int64_t>(sub.job));
  EXPECT_EQ(ctx_->Cancel(sub.job), MN_E_NOT_FOUND);
  ctx_->PokeJobs();
}

}  // namespace
