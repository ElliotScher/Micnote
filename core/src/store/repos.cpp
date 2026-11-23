#include "store/repos.hpp"

#include <algorithm>
#include <utility>

namespace micnote::store {

namespace {

// Unwraps a DbResult or returns its error from the enclosing function. Macros
// because they return from the caller; DbResult has no monadic "try".
// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses): `var`
// is a name being declared, which can't be parenthesized.
#define MN_TRY_ASSIGN(var, expr)             \
  auto var##_r = (expr);                     \
  if (!var##_r) {                            \
    return std::unexpected(var##_r.error()); \
  }                                          \
  auto &var = *var##_r

#define MN_TRY(expr)                      \
  if (auto mn_r = (expr); !mn_r) {        \
    return std::unexpected(mn_r.error()); \
  }
// NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)

DeviceId ToDeviceId(const std::vector<std::byte> &b) {
  DeviceId id{};
  std::copy_n(b.begin(), std::min(b.size(), id.size()), id.begin());
  return id;
}

// GCOVR_EXCL_BR_START: the Read* builders run in every test; their
// uncovered branches are gcc's exception cleanup for the string members.
Device ReadDevice(const Stmt &s) {
  return Device{.id = s.Int(0),
                .device_id = ToDeviceId(s.Blob(1)),
                .name = s.Text(2),
                .host_slot = s.OptInt(3),
                .paired_at = s.Int(4),
                .fw_version = s.Text(5),
                .last_seen = s.OptInt(6)};
}
// GCOVR_EXCL_BR_STOP

constexpr const char *kDeviceCols =
    "id, device_id, name, host_slot, paired_at, fw_version, last_seen";

constexpr std::array<const char *, 5> kStates = {"synced", "transcribing", "transcribed", "planned",
                                                 "error"};
constexpr std::array<const char *, 3> kConfidences = {"rtc", "repaired", "unknown"};

// The position of `s` among `names`. A name the table can't hold (its CHECK
// constraints and triggers keep anything else out) would be names.size().
// No named iterator: std::array's is a pointer in libstdc++ and libc++ but a
// class in MSVC's library.
template <std::size_t N>
std::size_t IndexOf(const std::array<const char *, N> &names, const std::string &s) {
  return static_cast<std::size_t>(
      std::ranges::distance(names.begin(), std::ranges::find(names, s)));
}

RecordingState ParseState(const std::string &s) {
  return static_cast<RecordingState>(IndexOf(kStates, s));
}

TimeConfidence ParseConfidence(const std::string &s) {
  return static_cast<TimeConfidence>(IndexOf(kConfidences, s));
}

constexpr const char *kRecordingCols =
    "id, device_id, rec_id, start_boot, start_uptime_ms, start_unix_ms, start_unix_ms_fixed, "
    "time_confidence, duration_ms, mnr_path, sha256, state, created_at";

// GCOVR_EXCL_BR_START: as for ReadDevice.
StoredRecording ReadRecording(const Stmt &s) {
  return StoredRecording{.id = s.Int(0),
                         .device = s.Int(1),
                         .rec_id = s.Int(2),
                         .start_boot = s.Int(3),
                         .start_uptime_ms = s.Int(4),
                         .start_unix_ms = s.Int(5),
                         .start_unix_ms_fixed = s.OptInt(6),
                         .time_confidence = ParseConfidence(s.Text(7)),
                         .duration_ms = s.Int(8),
                         .mnr_path = s.Text(9),
                         .sha256 = s.Blob(10),
                         .state = ParseState(s.Text(11)),
                         .created_at = s.Int(12),
                         .markers = {}};
}
// GCOVR_EXCL_BR_STOP

DbResult<std::vector<StoredMarker>> ReadMarkers(Db &db, std::int64_t recording) {
  MN_TRY_ASSIGN(s, db.Prepare("SELECT offset_ms, kind FROM markers WHERE recording_id = ? "
                              "ORDER BY offset_ms"));
  s.Bind(1, recording);
  std::vector<StoredMarker> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(StoredMarker{.offset_ms = s.Int(0), .kind = s.Int(1)});
  }
}

}  // namespace

const char *ToString(RecordingState s) { return kStates.at(static_cast<std::size_t>(s)); }
const char *ToString(TimeConfidence c) { return kConfidences.at(static_cast<std::size_t>(c)); }

// ---- devices ---------------------------------------------------------------

DbResult<std::int64_t> UpsertDevice(Db &db, const Device &d) {
  MN_TRY_ASSIGN(
      s, db.Prepare("INSERT INTO devices (device_id, name, host_slot, paired_at, fw_version, "
                    "last_seen) VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (device_id) DO UPDATE SET "
                    "name = excluded.name, host_slot = excluded.host_slot, "
                    "paired_at = excluded.paired_at, fw_version = excluded.fw_version, "
                    "last_seen = excluded.last_seen RETURNING id"));
  s.Bind(1, std::span<const std::byte>(d.device_id))
      .Bind(2, std::string_view(d.name))
      .Bind(3, d.host_slot)
      .Bind(4, d.paired_at)
      .Bind(5, std::string_view(d.fw_version))
      .Bind(6, d.last_seen);
  MN_TRY_ASSIGN(row, s.Step());
  static_cast<void>(row);  // RETURNING always gives the row
  return s.Int(0);
}

DbResult<std::optional<Device>> FindDevice(Db &db, const DeviceId &device_id) {
  MN_TRY_ASSIGN(
      s, db.Prepare(std::string("SELECT ") + kDeviceCols + " FROM devices WHERE device_id = ?"));
  s.Bind(1, std::span<const std::byte>(device_id));
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  return ReadDevice(s);
}

DbResult<std::vector<Device>> ListDevices(Db &db) {
  MN_TRY_ASSIGN(s, db.Prepare(std::string("SELECT ") + kDeviceCols + " FROM devices ORDER BY id"));
  std::vector<Device> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(ReadDevice(s));
  }
}

DbResult<void> TouchDevice(Db &db, std::int64_t id, std::int64_t last_seen,
                           const std::string &fw_version) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE devices SET last_seen = ?, fw_version = ? WHERE id = ?"));
  return s.Bind(1, last_seen).Bind(2, std::string_view(fw_version)).Bind(3, id).Run();
}

// ---- device_clock ------------------------------------------------------------

DbResult<void> AddClockObservation(Db &db, std::int64_t device, const ClockObservation &o) {
  MN_TRY_ASSIGN(s, db.Prepare("INSERT OR REPLACE INTO device_clock (device_id, boot_count, "
                              "uptime_ms, unix_ms, observed_at) VALUES (?, ?, ?, ?, ?)"));
  return s.Bind(1, device)
      .Bind(2, o.boot_count)
      .Bind(3, o.uptime_ms)
      .Bind(4, o.unix_ms)
      .Bind(5, o.observed_at)
      .Run();
}

DbResult<std::vector<ClockObservation>> ClockObservations(Db &db, std::int64_t device,
                                                          std::int64_t boot_count) {
  MN_TRY_ASSIGN(s, db.Prepare("SELECT boot_count, uptime_ms, unix_ms, observed_at FROM "
                              "device_clock WHERE device_id = ? AND boot_count = ? "
                              "ORDER BY uptime_ms"));
  s.Bind(1, device).Bind(2, boot_count);
  std::vector<ClockObservation> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(ClockObservation{.boot_count = s.Int(0),
                                   .uptime_ms = s.Int(1),
                                   .unix_ms = s.Int(2),
                                   .observed_at = s.Int(3)});
  }
}

// ---- recordings ----------------------------------------------------------------

DbResult<std::int64_t> InsertRecording(Db &db, const StoredRecording &r) {
  MN_TRY_ASSIGN(txn, Txn::Begin(db));
  MN_TRY_ASSIGN(s, db.Prepare("INSERT INTO recordings (device_id, rec_id, start_boot, "
                              "start_uptime_ms, start_unix_ms, start_unix_ms_fixed, "
                              "time_confidence, duration_ms, mnr_path, sha256, state, created_at) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
  s.Bind(1, r.device)
      .Bind(2, r.rec_id)
      .Bind(3, r.start_boot)
      .Bind(4, r.start_uptime_ms)
      .Bind(5, r.start_unix_ms)
      .Bind(6, r.start_unix_ms_fixed)
      .Bind(7, std::string_view(ToString(r.time_confidence)))
      .Bind(8, r.duration_ms)
      .Bind(9, std::string_view(r.mnr_path))
      .Bind(10, std::span<const std::byte>(r.sha256))
      .Bind(11, std::string_view(ToString(r.state)))
      .Bind(12, r.created_at);
  MN_TRY(s.Run());
  const std::int64_t id = db.LastInsertRowid();
  MN_TRY_ASSIGN(m,
                db.Prepare("INSERT INTO markers (recording_id, offset_ms, kind) VALUES (?, ?, ?)"));
  for (const StoredMarker &marker : r.markers) {
    m.Reset();
    MN_TRY(m.Bind(1, id).Bind(2, marker.offset_ms).Bind(3, marker.kind).Run());
  }
  MN_TRY(txn.Commit());
  return id;
}

DbResult<std::optional<StoredRecording>> GetRecording(Db &db, std::int64_t id) {
  MN_TRY_ASSIGN(
      s, db.Prepare(std::string("SELECT ") + kRecordingCols + " FROM recordings WHERE id = ?"));
  s.Bind(1, id);
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  StoredRecording r = ReadRecording(s);
  MN_TRY_ASSIGN(markers, ReadMarkers(db, id));
  r.markers = std::move(markers);
  return r;
}

DbResult<std::vector<StoredRecording>> ListRecordings(Db &db, std::size_t limit) {
  MN_TRY_ASSIGN(s, db.Prepare(std::string("SELECT ") + kRecordingCols +
                              " FROM recordings ORDER BY created_at DESC, id DESC LIMIT ?"));
  s.Bind(1, limit == 0 ? std::int64_t{-1} : static_cast<std::int64_t>(limit));
  std::vector<StoredRecording> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      break;
    }
    out.push_back(ReadRecording(s));
  }
  for (StoredRecording &r : out) {
    MN_TRY_ASSIGN(markers, ReadMarkers(db, r.id));
    r.markers = std::move(markers);
  }
  return out;
}

DbResult<void> SetRecordingState(Db &db, std::int64_t id, RecordingState state) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE recordings SET state = ? WHERE id = ?"));
  return s.Bind(1, std::string_view(ToString(state))).Bind(2, id).Run();
}

DbResult<void> SetRecordingTime(Db &db, std::int64_t id, std::optional<std::int64_t> fixed,
                                TimeConfidence confidence) {
  MN_TRY_ASSIGN(
      s, db.Prepare(
             "UPDATE recordings SET start_unix_ms_fixed = ?, time_confidence = ? WHERE id = ?"));
  return s.Bind(1, fixed).Bind(2, std::string_view(ToString(confidence))).Bind(3, id).Run();
}

DbResult<bool> DeleteRecording(Db &db, std::int64_t id) {
  MN_TRY_ASSIGN(s, db.Prepare("DELETE FROM recordings WHERE id = ?"));
  MN_TRY(s.Bind(1, id).Run());
  return db.Changes() > 0;
}

// ---- transcripts -------------------------------------------------------------

DbResult<std::int64_t> SaveTranscript(Db &db, const Transcript &t) {
  MN_TRY_ASSIGN(txn, Txn::Begin(db));
  // Segments (and their FTS rows, by trigger) cascade from the old one.
  MN_TRY_ASSIGN(del, db.Prepare("DELETE FROM transcripts WHERE recording_id = ?"));
  MN_TRY(del.Bind(1, t.recording).Run());
  MN_TRY_ASSIGN(s, db.Prepare("INSERT INTO transcripts (recording_id, model, backend, language, "
                              "created_at, rtf) VALUES (?, ?, ?, ?, ?, ?)"));
  MN_TRY(s.Bind(1, t.recording)
             .Bind(2, std::string_view(t.model))
             .Bind(3, std::string_view(t.backend))
             .Bind(4, std::string_view(t.language))
             .Bind(5, t.created_at)
             .Bind(6, t.rtf)
             .Run());
  const std::int64_t id = db.LastInsertRowid();
  MN_TRY_ASSIGN(seg, db.Prepare("INSERT INTO segments (transcript_id, idx, start_ms, end_ms, text, "
                                "avg_logprob, no_speech_prob) VALUES (?, ?, ?, ?, ?, ?, ?)"));
  for (std::size_t i = 0; i < t.segments.size(); ++i) {
    const Segment &g = t.segments[i];
    seg.Reset();
    MN_TRY(seg.Bind(1, id)
               .Bind(2, static_cast<std::int64_t>(i))
               .Bind(3, g.start_ms)
               .Bind(4, g.end_ms)
               .Bind(5, std::string_view(g.text))
               .Bind(6, g.avg_logprob)
               .Bind(7, g.no_speech_prob)
               .Run());
  }
  MN_TRY(txn.Commit());
  return id;
}

DbResult<std::optional<Transcript>> GetTranscript(Db &db, std::int64_t recording) {
  MN_TRY_ASSIGN(s, db.Prepare("SELECT id, recording_id, model, backend, language, created_at, rtf "
                              "FROM transcripts WHERE recording_id = ?"));
  s.Bind(1, recording);
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  // GCOVR_EXCL_BR_START: as for ReadDevice.
  Transcript t{.id = s.Int(0),
               .recording = s.Int(1),
               .model = s.Text(2),
               .backend = s.Text(3),
               .language = s.Text(4),
               .created_at = s.Int(5),
               .rtf = s.Real(6),
               .segments = {}};
  // GCOVR_EXCL_BR_STOP
  MN_TRY_ASSIGN(seg, db.Prepare("SELECT start_ms, end_ms, text, avg_logprob, no_speech_prob FROM "
                                "segments WHERE transcript_id = ? ORDER BY idx"));
  seg.Bind(1, t.id);
  for (;;) {
    MN_TRY_ASSIGN(more, seg.Step());
    if (!more) {
      return t;
    }
    // GCOVR_EXCL_BR_START: as for ReadDevice.
    t.segments.push_back(Segment{.start_ms = seg.Int(0),
                                 .end_ms = seg.Int(1),
                                 .text = seg.Text(2),
                                 .avg_logprob = seg.Real(3),
                                 .no_speech_prob = seg.Real(4)});
    // GCOVR_EXCL_BR_STOP
  }
}

DbResult<std::vector<SearchHit>> Search(Db &db, const std::string &query, std::size_t limit) {
  MN_TRY_ASSIGN(
      s, db.Prepare("SELECT t.recording_id, g.start_ms, g.end_ms, g.text FROM segments_fts f "
                    "JOIN segments g ON g.rowid = f.rowid "
                    "JOIN transcripts t ON t.id = g.transcript_id "
                    "WHERE segments_fts MATCH ? ORDER BY bm25(segments_fts) LIMIT ?"));
  s.Bind(1, std::string_view(query)).Bind(2, static_cast<std::int64_t>(limit));
  std::vector<SearchHit> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(SearchHit{
        .recording = s.Int(0), .start_ms = s.Int(1), .end_ms = s.Int(2), .text = s.Text(3)});
  }
}

// ---- jobs ---------------------------------------------------------------------

namespace {

constexpr std::array<const char *, 5> kJobStates = {"queued", "running", "done", "dead",
                                                    "cancelled"};
constexpr const char *kJobCols =
    "id, kind, ref_id, params_json, state, attempts, not_before, last_error, created_at, "
    "updated_at";

JobState ParseJobState(const std::string &s) {
  return static_cast<JobState>(IndexOf(kJobStates, s));
}

// GCOVR_EXCL_BR_START: as for ReadDevice.
Job ReadJob(const Stmt &s) {
  return Job{.id = s.Int(0),
             .kind = s.Text(1),
             .ref_id = s.OptInt(2),
             .params_json = s.Text(3),
             .state = ParseJobState(s.Text(4)),
             .attempts = s.Int(5),
             .not_before = s.Int(6),
             .last_error = s.Text(7),
             .created_at = s.Int(8),
             .updated_at = s.Int(9)};
}
// GCOVR_EXCL_BR_STOP

DbResult<void> SetJob(Db &db, std::int64_t id, const char *state, std::int64_t now,
                      std::optional<std::int64_t> not_before, const std::string *error) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE jobs SET state = ?, updated_at = ?, "
                              "not_before = coalesce(?, not_before), "
                              "last_error = coalesce(?, last_error) WHERE id = ?"));
  s.Bind(1, std::string_view(state)).Bind(2, now).Bind(3, not_before);
  if (error != nullptr) {
    s.Bind(4, std::string_view(*error));
  }
  return s.Bind(5, id).Run();
}

}  // namespace

const char *ToString(JobState s) { return kJobStates.at(static_cast<std::size_t>(s)); }

DbResult<std::int64_t> EnqueueJob(Db &db, const std::string &kind,
                                  std::optional<std::int64_t> ref_id,
                                  const std::string &params_json, std::int64_t now) {
  MN_TRY_ASSIGN(s, db.Prepare("INSERT INTO jobs (kind, ref_id, params_json, state, attempts, "
                              "not_before, created_at, updated_at) "
                              "VALUES (?, ?, ?, 'queued', 0, 0, ?, ?)"));
  MN_TRY(s.Bind(1, std::string_view(kind))
             .Bind(2, ref_id)
             .Bind(3, std::string_view(params_json))
             .Bind(4, now)
             .Bind(5, now)
             .Run());
  return db.LastInsertRowid();
}

DbResult<std::optional<Job>> ClaimJob(Db &db, const std::string &kind, std::int64_t now) {
  MN_TRY_ASSIGN(s, db.Prepare(std::string("UPDATE jobs SET state = 'running', attempts = attempts "
                                          "+ 1, updated_at = ? WHERE id = (SELECT id FROM jobs "
                                          "WHERE kind = ? AND state = 'queued' AND not_before <= "
                                          "? ORDER BY id LIMIT 1) RETURNING ") +
                              kJobCols));
  s.Bind(1, now).Bind(2, std::string_view(kind)).Bind(3, now);
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  Job j = ReadJob(s);
  MN_TRY(s.Run());  // finish the statement
  return j;
}

DbResult<std::optional<Job>> GetJob(Db &db, std::int64_t id) {
  MN_TRY_ASSIGN(s, db.Prepare(std::string("SELECT ") + kJobCols + " FROM jobs WHERE id = ?"));
  s.Bind(1, id);
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  return ReadJob(s);
}

DbResult<std::vector<Job>> ListJobs(Db &db, std::size_t limit) {
  MN_TRY_ASSIGN(
      s, db.Prepare(std::string("SELECT ") + kJobCols + " FROM jobs ORDER BY id DESC LIMIT ?"));
  s.Bind(1, limit == 0 ? std::int64_t{-1} : static_cast<std::int64_t>(limit));
  std::vector<Job> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(ReadJob(s));
  }
}

DbResult<void> FinishJob(Db &db, std::int64_t id, std::int64_t now) {
  return SetJob(db, id, "done", now, std::nullopt, nullptr);
}

DbResult<void> RetryJob(Db &db, std::int64_t id, std::int64_t not_before, const std::string &error,
                        std::int64_t now) {
  return SetJob(db, id, "queued", now, not_before, &error);
}

DbResult<void> KillJob(Db &db, std::int64_t id, const std::string &error, std::int64_t now) {
  return SetJob(db, id, "dead", now, std::nullopt, &error);
}

DbResult<bool> CancelQueuedJob(Db &db, std::int64_t id, std::int64_t now) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE jobs SET state = 'cancelled', updated_at = ? "
                              "WHERE id = ? AND state = 'queued'"));
  MN_TRY(s.Bind(1, now).Bind(2, id).Run());
  return db.Changes() > 0;
}

DbResult<void> MarkJobCancelled(Db &db, std::int64_t id, std::int64_t now) {
  return SetJob(db, id, "cancelled", now, std::nullopt, nullptr);
}

DbResult<void> RequeueStoppedJob(Db &db, std::int64_t id, std::int64_t now) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE jobs SET state = 'queued', not_before = 0, "
                              "attempts = max(attempts - 1, 0), updated_at = ? WHERE id = ?"));
  return s.Bind(1, now).Bind(2, id).Run();
}

DbResult<int> RequeueRunningJobs(Db &db, std::int64_t now) {
  MN_TRY_ASSIGN(s, db.Prepare("UPDATE jobs SET state = 'queued', updated_at = ? "
                              "WHERE state = 'running'"));
  MN_TRY(s.Bind(1, now).Run());
  return db.Changes();
}

DbResult<std::optional<std::int64_t>> NextDue(Db &db, const std::string &kind) {
  MN_TRY_ASSIGN(s,
                db.Prepare("SELECT min(not_before) FROM jobs WHERE kind = ? AND state = 'queued'"));
  s.Bind(1, std::string_view(kind));
  MN_TRY_ASSIGN(row, s.Step());
  static_cast<void>(row);  // an aggregate always gives a row
  return s.OptInt(0);
}

DbResult<std::optional<std::vector<std::byte>>> FindRecordingSha(Db &db, std::int64_t device,
                                                                 std::int64_t rec_id) {
  MN_TRY_ASSIGN(s, db.Prepare("SELECT sha256 FROM recordings WHERE device_id = ? AND rec_id = ?"));
  s.Bind(1, device).Bind(2, rec_id);
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  return s.Blob(0);
}

DbResult<std::vector<StoredRecording>> UndatedRecordings(Db &db, std::int64_t device,
                                                         std::int64_t boot) {
  MN_TRY_ASSIGN(s, db.Prepare(std::string("SELECT ") + kRecordingCols +
                              " FROM recordings WHERE device_id = ? AND start_boot = ? "
                              "AND time_confidence = 'unknown' ORDER BY id"));
  s.Bind(1, device).Bind(2, boot);
  std::vector<StoredRecording> out;
  for (;;) {
    MN_TRY_ASSIGN(row, s.Step());
    if (!row) {
      return out;
    }
    out.push_back(ReadRecording(s));
  }
}

// ---- settings ----------------------------------------------------------------

DbResult<std::optional<std::string>> GetSetting(Db &db, const std::string &key) {
  MN_TRY_ASSIGN(s, db.Prepare("SELECT value_json FROM settings WHERE key = ?"));
  s.Bind(1, std::string_view(key));
  MN_TRY_ASSIGN(row, s.Step());
  if (!row) {
    return std::nullopt;
  }
  return s.Text(0);
}

DbResult<void> SetSetting(Db &db, const std::string &key, const std::string &value_json) {
  MN_TRY_ASSIGN(s, db.Prepare("INSERT INTO settings (key, value_json) VALUES (?, ?) "
                              "ON CONFLICT (key) DO UPDATE SET value_json = excluded.value_json"));
  return s.Bind(1, std::string_view(key)).Bind(2, std::string_view(value_json)).Run();
}

}  // namespace micnote::store
