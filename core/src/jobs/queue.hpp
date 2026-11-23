// The persistent job queue (stage 2 §4.8). Jobs are rows of the store's
// `jobs` table; one worker thread per kind runs them, one at a time:
//
//   queued -> running -> done
//                     -> queued again after 2^attempts minutes (retryable)
//                     -> dead (not retryable, or the 5th failure)
//   queued | running -> cancelled (Cancel)
//
// At Start(), jobs a crash left `running` go back to the queue. A worker
// touches the database only to claim and settle a job (through DbAccess,
// which serializes it with the store's other users), never while the job
// runs. Stop() asks running jobs to stop; one that returns kStopped is put
// back in the queue without counting the attempt, so it resumes next time.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "store/db.hpp"
#include "store/repos.hpp"

namespace micnote::jobs {

inline constexpr int kMaxAttempts = 5;

struct JobError {
  enum class Kind : std::uint8_t {
    kRetryable,  // worth trying again later (a puck out of reach, a busy link)
    kFatal,      // won't get better (not paired, a corrupt file)
    kStopped,    // Cancelled() was true: cancelled, or the queue is stopping
  };
  Kind kind = Kind::kFatal;
  std::string message;
};

using JobResult = std::expected<void, JobError>;

// Runs fn with the database, serialized with every other user of it.
using DbAccess = std::function<void(const std::function<void(store::Db &)> &fn)>;

// What a running job sees.
class JobContext {
 public:
  // job, db and cancelled must outlive the context.
  JobContext(const store::Job &job, const DbAccess &db,
             std::function<void(const std::string &)> emit, const std::atomic<bool> &cancelled,
             std::function<std::int64_t()> now)
      : job_(&job),
        db_(&db),
        emit_(std::move(emit)),
        cancelled_(&cancelled),
        now_(std::move(now)) {}

  [[nodiscard]] const store::Job &job() const { return *job_; }
  void WithDb(const std::function<void(store::Db &)> &fn) const { (*db_)(fn); }
  [[nodiscard]] bool Cancelled() const { return cancelled_->load(); }
  // Emits {"type":"job.progress","job":id,"kind":...,"pct":pct}.
  void Progress(int pct) const;
  void Emit(const std::string &event_json) const { emit_(event_json); }
  [[nodiscard]] std::int64_t Now() const { return now_(); }

 private:
  const store::Job *job_;
  const DbAccess *db_;
  std::function<void(const std::string &)> emit_;
  const std::atomic<bool> *cancelled_;
  std::function<std::int64_t()> now_;
};

using Handler = std::function<JobResult(JobContext &)>;

class JobQueue {
 public:
  // now() is Unix time in ms (injected for tests).
  JobQueue(DbAccess db, std::function<void(const std::string &)> emit,
           std::function<std::int64_t()> now);
  JobQueue(const JobQueue &) = delete;
  JobQueue &operator=(const JobQueue &) = delete;
  JobQueue(JobQueue &&) = delete;
  JobQueue &operator=(JobQueue &&) = delete;
  ~JobQueue();

  // Before Start(): the kinds this queue runs.
  void Register(const std::string &kind, Handler handler);
  [[nodiscard]] bool Handles(const std::string &kind) const;

  // Requeues crashed jobs and starts the workers. Returns the store error
  // if the requeue failed (the workers start anyway).
  store::DbResult<void> Start();
  // Asks running jobs to stop and waits for the workers.
  void Stop();

  // Queues a job of a registered kind; returns its id.
  store::DbResult<std::int64_t> Submit(const std::string &kind, std::optional<std::int64_t> ref_id,
                                       const std::string &params_json);
  // Cancels a queued or running job. False if it's neither (or unknown).
  bool Cancel(std::int64_t id);
  // Wakes the workers to look for due jobs now (after the clock moved, in
  // tests; the workers also wake by themselves when a retry falls due).
  void Poke();
  // While a kind is paused its worker starts nothing new (a running job
  // finishes); stage 5 holds transcription until the OS grants background
  // time. Jobs stay queued and can still be submitted and cancelled.
  void SetPaused(const std::string &kind, bool paused);

 private:
  enum class Ran : std::uint8_t { kJob, kIdle, kStoreError };
  void Work(const std::string &kind);
  Ran RunOne(const std::string &kind);

  DbAccess db_;
  std::function<void(const std::string &)> emit_;
  std::function<std::int64_t()> now_;
  std::map<std::string, Handler> handlers_;
  std::vector<std::thread> workers_;

  std::mutex mu_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::uint64_t pokes_ = 0;
  std::set<std::string> paused_;
  // Running jobs' cancel flags, by id.
  std::map<std::int64_t, std::shared_ptr<std::atomic<bool>>> running_;
};

// When a job that failed for the `attempts`-th time is tried again:
// 2^attempts minutes after now.
std::int64_t RetryAt(std::int64_t now, std::int64_t attempts);

}  // namespace micnote::jobs
