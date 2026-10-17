"""Bounded, content-free stage timestamps in the job's existing transaction."""
import time
import uuid


class JobTimings:
    def __init__(self, db):
        self.db = db
        self.boot_id = uuid.uuid4().hex
        db.execute("""CREATE TABLE IF NOT EXISTS job_events (
            request_id TEXT NOT NULL, stage TEXT NOT NULL, boot_id TEXT NOT NULL,
            wall_time REAL NOT NULL, monotonic_ns INTEGER NOT NULL,
            duration_ms REAL, attempt INTEGER NOT NULL DEFAULT 0)""")
        db.execute("CREATE INDEX IF NOT EXISTS job_events_request ON job_events(request_id)")

    def record(self, request_id, stage, *, duration_ms=None, attempt=0):
        self.db.execute("INSERT INTO job_events VALUES(?,?,?,?,?,?,?)",
                        (request_id, stage, self.boot_id, time.time(), time.monotonic_ns(),
                         duration_ms, attempt))
        self.db.execute("""DELETE FROM job_events WHERE request_id=? AND rowid NOT IN
            (SELECT rowid FROM job_events WHERE request_id=? ORDER BY rowid DESC LIMIT 64)""",
                        (request_id, request_id))
