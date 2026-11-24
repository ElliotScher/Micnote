-- Stage 3: items keep the words they came from, and deleting a recording
-- takes its items and drafts with it (what was executed stays in executions
-- and audit). Nothing wrote these tables before this version, so they can be
-- rebuilt; SQLite can't change a foreign key in place.
ALTER TABLE items ADD COLUMN said TEXT NOT NULL DEFAULT '';

DROP TABLE executions;
DROP TABLE action_revisions;
DROP TABLE actions;

CREATE TABLE actions (
  id INTEGER PRIMARY KEY,
  item_id INTEGER NOT NULL REFERENCES items ON DELETE CASCADE,
  connector TEXT NOT NULL,
  action_type TEXT NOT NULL,
  -- 'draft' until stage 4 confirms or discards it
  state TEXT NOT NULL,
  current_revision INTEGER NOT NULL,
  idempotency_key TEXT UNIQUE NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE INDEX actions_item ON actions (item_id);

CREATE TABLE action_revisions (
  action_id INTEGER NOT NULL REFERENCES actions ON DELETE CASCADE,
  rev INTEGER NOT NULL,
  author TEXT NOT NULL CHECK (author IN ('planner_local', 'planner_cloud', 'user')),
  payload_json TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY (action_id, rev));

CREATE TABLE executions (
  id INTEGER PRIMARY KEY,
  action_id INTEGER REFERENCES actions ON DELETE SET NULL,
  rev INTEGER,
  started_at INTEGER,
  finished_at INTEGER,
  outcome TEXT,
  result_json TEXT,
  error TEXT);

CREATE INDEX items_recording ON items (recording_id);
