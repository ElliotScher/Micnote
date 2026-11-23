-- Stage 2 PR 3, the job queue (§4.8): a job's request (the USB port for a
-- sync, ...), and the states it moves through.
ALTER TABLE jobs ADD COLUMN params_json TEXT NOT NULL DEFAULT '{}';
-- queued -> running -> done | queued again (retry, after not_before) | dead
-- (out of retries, or not retryable) | cancelled.
CREATE TRIGGER jobs_state_check BEFORE INSERT ON jobs
  WHEN new.state NOT IN ('queued', 'running', 'done', 'dead', 'cancelled')
BEGIN
  SELECT RAISE(ABORT, 'bad job state');
END;
CREATE TRIGGER jobs_state_check_update BEFORE UPDATE OF state ON jobs
  WHEN new.state NOT IN ('queued', 'running', 'done', 'dead', 'cancelled')
BEGIN
  SELECT RAISE(ABORT, 'bad job state');
END;
