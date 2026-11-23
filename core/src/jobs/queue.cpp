#include "jobs/queue.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

namespace micnote::jobs {

namespace {

using nlohmann::json;

// How long an idle worker sleeps at most before looking again.
constexpr auto kIdle = std::chrono::seconds(60);
// After a store error while claiming, before trying again.
constexpr auto kStoreBackoff = std::chrono::seconds(1);

}  // namespace

void JobContext::Progress(int pct) const {
  // GCOVR_EXCL_BR_START: nlohmann/json's exception-cleanup branches.
  emit_(
      json{{"type", "job.progress"}, {"job", job_->id}, {"kind", job_->kind}, {"pct", pct}}.dump());
  // GCOVR_EXCL_BR_STOP
}

std::int64_t RetryAt(std::int64_t now, std::int64_t attempts) {
  const auto doublings = static_cast<unsigned>(std::clamp<std::int64_t>(attempts, 0, 16));
  return now + (static_cast<std::int64_t>(1U << doublings) * 60 * 1000);
}

JobQueue::JobQueue(DbAccess db, std::function<void(const std::string &)> emit,
                   std::function<std::int64_t()> now)
    : db_(std::move(db)), emit_(std::move(emit)), now_(std::move(now)) {}

JobQueue::~JobQueue() { Stop(); }

void JobQueue::Register(const std::string &kind, Handler handler) {
  handlers_[kind] = std::move(handler);
}

bool JobQueue::Handles(const std::string &kind) const { return handlers_.contains(kind); }

store::DbResult<void> JobQueue::Start() {
  store::DbResult<void> result;
  db_([&](store::Db &db) {
    if (auto n = store::RequeueRunningJobs(db, now_()); !n) {
      result = std::unexpected(n.error());
    }
  });
  for (const auto &[kind, handler] : handlers_) {
    workers_.emplace_back([this, k = kind] { Work(k); });
  }
  return result;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `result`

void JobQueue::Stop() {
  {
    const std::scoped_lock lock(mu_);
    stopping_ = true;
    for (auto &[id, flag] : running_) {
      flag->store(true);
    }
  }
  wake_.notify_all();
  for (auto &w : workers_) {
    w.join();
  }
  workers_.clear();
}

store::DbResult<std::int64_t> JobQueue::Submit(const std::string &kind,
                                               std::optional<std::int64_t> ref_id,
                                               const std::string &params_json) {
  store::DbResult<std::int64_t> id = std::unexpected(store::DbError{.code = 0, .message = ""});
  db_([&](store::Db &db) { id = store::EnqueueJob(db, kind, ref_id, params_json, now_()); });
  if (id) {
    Poke();
  }
  return id;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `id`

bool JobQueue::Cancel(std::int64_t id) {
  bool cancelled = false;
  db_([&](store::Db &db) {
    const auto r = store::CancelQueuedJob(db, id, now_());
    cancelled = r && *r;
  });
  if (cancelled) {
    emit_(json{{"type", "job.cancelled"}, {"job", id}}.dump());  // GCOVR_EXCL_BR_LINE: see Progress
    return true;
  }
  const std::scoped_lock lock(mu_);
  const auto it = running_.find(id);
  if (it == running_.end()) {
    return false;
  }
  it->second->store(true);
  return true;
}

void JobQueue::SetPaused(const std::string &kind, bool paused) {
  {
    const std::scoped_lock lock(mu_);
    if (paused) {
      paused_.insert(kind);
    } else {
      paused_.erase(kind);
    }
    ++pokes_;
  }
  wake_.notify_all();
}

void JobQueue::Poke() {
  {
    const std::scoped_lock lock(mu_);
    ++pokes_;
  }
  wake_.notify_all();
}

// Claims and runs one due job of `kind`.
JobQueue::Ran JobQueue::RunOne(const std::string &kind) {
  std::optional<store::Job> job;
  bool store_ok = true;
  db_([&](store::Db &db) {
    auto claimed = store::ClaimJob(db, kind, now_());
    store_ok = claimed.has_value();
    if (claimed) {
      job = std::move(*claimed);
    }
  });
  if (!store_ok) {
    return Ran::kStoreError;
  }
  if (!job) {
    return Ran::kIdle;
  }
  auto flag = std::make_shared<std::atomic<bool>>(false);
  {
    const std::scoped_lock lock(mu_);
    flag->store(stopping_);
    running_[job->id] = flag;
  }
  JobContext ctx(*job, db_, emit_, *flag, now_);
  const JobResult result = handlers_.at(kind)(ctx);
  bool stopping = false;
  {
    const std::scoped_lock lock(mu_);
    running_.erase(job->id);
    stopping = stopping_;
  }

  const std::int64_t now = now_();
  json event{{"job", job->id}, {"kind", kind}};  // GCOVR_EXCL_BR_LINE: see Progress
  store::DbResult<void> settled;
  db_([&](store::Db &db) {
    if (result) {
      event["type"] = "job.done";
      settled = store::FinishJob(db, job->id, now);
      return;
    }
    const JobError &e = result.error();
    if (e.kind == JobError::Kind::kStopped && stopping) {
      event["type"] = "job.paused";
      settled = store::RequeueStoppedJob(db, job->id, now);
      return;
    }
    if (e.kind == JobError::Kind::kStopped) {
      event["type"] = "job.cancelled";
      settled = store::MarkJobCancelled(db, job->id, now);
      return;
    }
    event["type"] = "job.failed";
    event["error"] = e.message;
    if (e.kind == JobError::Kind::kRetryable && job->attempts < kMaxAttempts) {
      const std::int64_t at = RetryAt(now, job->attempts);
      event["retry_at"] = at;
      settled = store::RetryJob(db, job->id, at, e.message, now);
    } else {
      event["retry_at"] = nullptr;
      settled = store::KillJob(db, job->id, e.message, now);
    }
  });
  if (!settled) {
    // The row stays `running`; the next Start() puts it back in the queue.
    // GCOVR_EXCL_BR_START: see Progress
    emit_(json{{"type", "job.store_error"}, {"job", job->id}, {"error", settled.error().message}}
              .dump());
    // GCOVR_EXCL_BR_STOP
  }
  emit_(event.dump());
  return Ran::kJob;
}

void JobQueue::Work(const std::string &kind) {
  for (;;) {
    std::uint64_t seen = 0;
    {
      std::unique_lock lock(mu_);
      // Paused: wait for a resume (or anything else that pokes).
      while (!stopping_ && paused_.contains(kind)) {
        const std::uint64_t before = pokes_;
        wake_.wait(lock, [&] { return stopping_ || pokes_ != before; });
      }
      if (stopping_) {
        return;
      }
      seen = pokes_;
    }
    const Ran ran = RunOne(kind);
    if (ran == Ran::kJob) {
      continue;
    }
    // Nothing due: sleep until poked, stopped, or the next retry is due.
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(kIdle);
    if (ran == Ran::kStoreError) {
      wait = std::chrono::duration_cast<std::chrono::milliseconds>(kStoreBackoff);
    } else {
      // A store error here just means sleeping the full idle time.
      std::optional<std::int64_t> due;
      db_([&](store::Db &db) { due = store::NextDue(db, kind).value_or(std::nullopt); });
      if (due) {
        wait = std::clamp(std::chrono::milliseconds(*due - now_()), std::chrono::milliseconds(0),
                          wait);
      }
    }
    std::unique_lock lock(mu_);
    wake_.wait_for(lock, wait, [&] { return stopping_ || pokes_ != seen; });
  }
}

}  // namespace micnote::jobs
