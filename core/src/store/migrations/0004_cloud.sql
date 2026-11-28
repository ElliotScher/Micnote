-- Stage 3, the cloud path: an item keeps the action the planner chose for it
-- ("email.send_email"), even when the local draft failed, so it can be
-- drafted again (with Claude) later. '' for a plain note.
ALTER TABLE items ADD COLUMN intent TEXT NOT NULL DEFAULT '';
