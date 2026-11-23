// The store's tables as typed calls (stage 2 §4.3). Each function is one
// statement or one transaction; callers hold the Db (the store's writer
// thread, or a reader).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "store/db.hpp"

namespace micnote::store {

using DeviceId = std::array<std::byte, 8>;

// ---- devices ---------------------------------------------------------------

struct Device {
  std::int64_t id = 0;
  DeviceId device_id{};
  std::string name;
  std::optional<std::int64_t> host_slot;
  std::int64_t paired_at = 0;
  std::string fw_version;
  std::optional<std::int64_t> last_seen;
};

// Inserts or updates by device_id; returns the row id.
DbResult<std::int64_t> UpsertDevice(Db &db, const Device &d);
DbResult<std::optional<Device>> FindDevice(Db &db, const DeviceId &device_id);
DbResult<std::vector<Device>> ListDevices(Db &db);
DbResult<void> TouchDevice(Db &db, std::int64_t id, std::int64_t last_seen,
                           const std::string &fw_version);

// ---- device_clock ------------------------------------------------------------

struct ClockObservation {
  std::int64_t boot_count = 0;
  std::int64_t uptime_ms = 0;
  std::int64_t unix_ms = 0;
  std::int64_t observed_at = 0;
};

DbResult<void> AddClockObservation(Db &db, std::int64_t device, const ClockObservation &o);
// The observations for one boot, oldest first.
DbResult<std::vector<ClockObservation>> ClockObservations(Db &db, std::int64_t device,
                                                          std::int64_t boot_count);

// ---- recordings ----------------------------------------------------------------

enum class TimeConfidence : std::uint8_t { kRtc, kRepaired, kUnknown };
enum class RecordingState : std::uint8_t {
  kSynced,
  kTranscribing,
  kTranscribed,
  kPlanned,
  kError,
};

struct StoredMarker {
  std::int64_t offset_ms = 0;
  std::int64_t kind = 0;
};

struct StoredRecording {
  std::int64_t id = 0;
  std::int64_t device = 0;  // devices.id
  std::int64_t rec_id = 0;  // the puck's ID (stored as a signed 64-bit value)
  std::int64_t start_boot = 0;
  std::int64_t start_uptime_ms = 0;
  std::int64_t start_unix_ms = 0;
  std::optional<std::int64_t> start_unix_ms_fixed;
  TimeConfidence time_confidence = TimeConfidence::kUnknown;
  std::int64_t duration_ms = 0;
  std::string mnr_path;
  std::vector<std::byte> sha256;
  RecordingState state = RecordingState::kSynced;
  std::int64_t created_at = 0;
  std::vector<StoredMarker> markers;
};

// Inserts a recording and its markers in one transaction; returns the row
// id. A (device, rec_id) that's already stored is a constraint error
// (SQLITE_CONSTRAINT_UNIQUE): a re-synced recording isn't stored twice.
DbResult<std::int64_t> InsertRecording(Db &db, const StoredRecording &r);
DbResult<std::optional<StoredRecording>> GetRecording(Db &db, std::int64_t id);
// Newest first; limit 0 means all.
DbResult<std::vector<StoredRecording>> ListRecordings(Db &db, std::size_t limit = 0);
DbResult<void> SetRecordingState(Db &db, std::int64_t id, RecordingState state);
DbResult<void> SetRecordingTime(Db &db, std::int64_t id, std::optional<std::int64_t> fixed,
                                TimeConfidence confidence);
// Deletes the row (markers and transcript go with it); the caller deletes
// the file. Returns false if there was no such recording.
DbResult<bool> DeleteRecording(Db &db, std::int64_t id);

const char *ToString(RecordingState s);
const char *ToString(TimeConfidence c);

// ---- transcripts -------------------------------------------------------------

struct Segment {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
  double avg_logprob = 0;
  double no_speech_prob = 0;
};

struct Transcript {
  std::int64_t id = 0;
  std::int64_t recording = 0;
  std::string model;
  std::string backend;
  std::string language;
  std::int64_t created_at = 0;
  double rtf = 0;
  std::vector<Segment> segments;
};

// Replaces any earlier transcript of the same recording (re-transcribing
// with another model); returns the transcript id.
DbResult<std::int64_t> SaveTranscript(Db &db, const Transcript &t);
DbResult<std::optional<Transcript>> GetTranscript(Db &db, std::int64_t recording);

struct SearchHit {
  std::int64_t recording = 0;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string text;
};

// Full-text search over segments (FTS5 query syntax), best matches first.
DbResult<std::vector<SearchHit>> Search(Db &db, const std::string &query, std::size_t limit = 50);

// ---- jobs (stage 2 §4.8) ---------------------------------------------------------

enum class JobState : std::uint8_t { kQueued, kRunning, kDone, kDead, kCancelled };

struct Job {
  std::int64_t id = 0;
  std::string kind;
  std::optional<std::int64_t> ref_id;
  std::string params_json = "{}";
  JobState state = JobState::kQueued;
  std::int64_t attempts = 0;
  std::int64_t not_before = 0;
  std::string last_error;
  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;
};

DbResult<std::int64_t> EnqueueJob(Db &db, const std::string &kind,
                                  std::optional<std::int64_t> ref_id,
                                  const std::string &params_json, std::int64_t now);
// The oldest queued job of `kind` due by `now`, now marked running with one
// more attempt; nullopt if there is none.
DbResult<std::optional<Job>> ClaimJob(Db &db, const std::string &kind, std::int64_t now);
DbResult<std::optional<Job>> GetJob(Db &db, std::int64_t id);
// Newest first; limit 0 means all.
DbResult<std::vector<Job>> ListJobs(Db &db, std::size_t limit = 0);
DbResult<void> FinishJob(Db &db, std::int64_t id, std::int64_t now);
// Back to queued until not_before, or dead (with the error either way).
DbResult<void> RetryJob(Db &db, std::int64_t id, std::int64_t not_before, const std::string &error,
                        std::int64_t now);
DbResult<void> KillJob(Db &db, std::int64_t id, const std::string &error, std::int64_t now);
// A queued job is cancelled; returns false if it wasn't queued.
DbResult<bool> CancelQueuedJob(Db &db, std::int64_t id, std::int64_t now);
DbResult<void> MarkJobCancelled(Db &db, std::int64_t id, std::int64_t now);
// A job stopped by shutdown: queued again, due at once, this attempt not
// counted.
DbResult<void> RequeueStoppedJob(Db &db, std::int64_t id, std::int64_t now);
// At startup: jobs a crash left running go back to the queue.
DbResult<int> RequeueRunningJobs(Db &db, std::int64_t now);
// The earliest not_before among queued jobs of `kind`, if any.
DbResult<std::optional<std::int64_t>> NextDue(Db &db, const std::string &kind);

const char *ToString(JobState s);

// The sha256 of the stored recording of this device with this puck ID, if
// there is one.
DbResult<std::optional<std::vector<std::byte>>> FindRecordingSha(Db &db, std::int64_t device,
                                                                 std::int64_t rec_id);
// Recordings of a device's boot whose start time is still unknown.
DbResult<std::vector<StoredRecording>> UndatedRecordings(Db &db, std::int64_t device,
                                                         std::int64_t boot);

// ---- settings ----------------------------------------------------------------

DbResult<std::optional<std::string>> GetSetting(Db &db, const std::string &key);
DbResult<void> SetSetting(Db &db, const std::string &key, const std::string &value_json);

}  // namespace micnote::store
