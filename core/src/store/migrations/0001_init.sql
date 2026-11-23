-- Stage 2 §4.3: the whole schema, stage 3/4 tables included, so it is
-- reviewed once. Differences from the sketch there:
-- - devices is keyed by device_id (DeviceInfo, 8 bytes): since pairing uses
--   a fresh device key each time (protocol section 8), there is no
--   long-term device_pub. Link keys are secrets, not rows.
-- - recordings.mnr_path, not opus_path: the store keeps the original
--   encrypted .mnr (the §4.3 decision) and decrypts on demand.
-- - Triggers keep the external-content FTS index in step with segments.

CREATE TABLE devices (
  id INTEGER PRIMARY KEY,
  device_id BLOB UNIQUE NOT NULL CHECK (length(device_id) = 8),
  name TEXT,
  host_slot INTEGER,
  paired_at INTEGER NOT NULL,
  fw_version TEXT,
  last_seen INTEGER);

-- (boot, uptime) <-> wall clock, observed at each Hello + GetStatus, for
-- repairing the start time of recordings made while the RTC was unset.
CREATE TABLE device_clock (
  device_id INTEGER NOT NULL REFERENCES devices ON DELETE CASCADE,
  boot_count INTEGER NOT NULL,
  uptime_ms INTEGER NOT NULL,
  unix_ms INTEGER NOT NULL,
  observed_at INTEGER NOT NULL,
  PRIMARY KEY (device_id, boot_count, uptime_ms));

CREATE TABLE recordings (
  id INTEGER PRIMARY KEY,
  device_id INTEGER NOT NULL REFERENCES devices,
  rec_id INTEGER NOT NULL,
  start_boot INTEGER,
  start_uptime_ms INTEGER,
  start_unix_ms INTEGER,                    -- as reported (0: RTC unset)
  start_unix_ms_fixed INTEGER,
  time_confidence TEXT NOT NULL CHECK (time_confidence IN ('rtc', 'repaired', 'unknown')),
  duration_ms INTEGER,
  mnr_path TEXT NOT NULL,
  sha256 BLOB,
  state TEXT NOT NULL CHECK (state IN ('synced', 'transcribing', 'transcribed', 'planned', 'error')),
  created_at INTEGER NOT NULL,
  UNIQUE (device_id, rec_id));

CREATE TABLE markers (
  recording_id INTEGER NOT NULL REFERENCES recordings ON DELETE CASCADE,
  offset_ms INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  PRIMARY KEY (recording_id, offset_ms));

CREATE TABLE transcripts (
  id INTEGER PRIMARY KEY,
  recording_id INTEGER UNIQUE NOT NULL REFERENCES recordings ON DELETE CASCADE,
  model TEXT,
  backend TEXT,
  language TEXT,
  created_at INTEGER NOT NULL,
  rtf REAL);

CREATE TABLE segments (
  transcript_id INTEGER NOT NULL REFERENCES transcripts ON DELETE CASCADE,
  idx INTEGER NOT NULL,
  start_ms INTEGER NOT NULL,
  end_ms INTEGER NOT NULL,
  text TEXT NOT NULL,
  avg_logprob REAL,
  no_speech_prob REAL,
  PRIMARY KEY (transcript_id, idx));

CREATE VIRTUAL TABLE segments_fts USING fts5(text, content='segments', content_rowid='rowid');
CREATE TRIGGER segments_ai AFTER INSERT ON segments BEGIN
  INSERT INTO segments_fts(rowid, text) VALUES (new.rowid, new.text);
END;
CREATE TRIGGER segments_ad AFTER DELETE ON segments BEGIN
  INSERT INTO segments_fts(segments_fts, rowid, text) VALUES ('delete', old.rowid, old.text);
END;
CREATE TRIGGER segments_au AFTER UPDATE OF text ON segments BEGIN
  INSERT INTO segments_fts(segments_fts, rowid, text) VALUES ('delete', old.rowid, old.text);
  INSERT INTO segments_fts(rowid, text) VALUES (new.rowid, new.text);
END;

-- Stages 3 and 4.
CREATE TABLE items (
  id INTEGER PRIMARY KEY,
  recording_id INTEGER REFERENCES recordings ON DELETE CASCADE,
  kind TEXT,
  start_ms INTEGER,
  end_ms INTEGER,
  text TEXT,
  created_at INTEGER);

CREATE TABLE actions (
  id INTEGER PRIMARY KEY,
  item_id INTEGER REFERENCES items,
  connector TEXT NOT NULL,
  action_type TEXT NOT NULL,
  state TEXT NOT NULL,
  current_revision INTEGER,
  idempotency_key TEXT UNIQUE NOT NULL,
  created_at INTEGER,
  updated_at INTEGER);

CREATE TABLE action_revisions (
  action_id INTEGER NOT NULL REFERENCES actions,
  rev INTEGER NOT NULL,
  author TEXT CHECK (author IN ('planner_local', 'planner_cloud', 'user')),
  payload_json TEXT NOT NULL,
  created_at INTEGER,
  PRIMARY KEY (action_id, rev));

CREATE TABLE executions (
  id INTEGER PRIMARY KEY,
  action_id INTEGER REFERENCES actions,
  rev INTEGER,
  started_at INTEGER,
  finished_at INTEGER,
  outcome TEXT,
  result_json TEXT,
  error TEXT);

CREATE TABLE audit (
  id INTEGER PRIMARY KEY,
  at INTEGER NOT NULL,
  actor TEXT,
  event TEXT,
  detail_json TEXT);

CREATE TABLE jobs (
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL,
  ref_id INTEGER,
  state TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  not_before INTEGER NOT NULL DEFAULT 0,
  last_error TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE INDEX jobs_runnable ON jobs (state, not_before);

CREATE TABLE settings (
  key TEXT PRIMARY KEY,
  value_json TEXT NOT NULL);
