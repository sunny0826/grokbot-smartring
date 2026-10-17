#!/usr/bin/env python3
"""Read stage timings without opening the mailbox or exposing speech and credentials."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sqlite3


def timelines(database, limit=5):
    uri = Path(database).resolve().as_uri() + "?mode=ro"
    with sqlite3.connect(uri, uri=True) as db:
        db.row_factory = sqlite3.Row
        jobs = db.execute("""SELECT request_id,status,(length(audio)-44)/96000.0 AS audio_seconds
            FROM jobs ORDER BY rowid DESC LIMIT ?""", (limit,)).fetchall()
        has_events = db.execute("SELECT 1 FROM sqlite_master WHERE name='job_events'").fetchone()
        result = []
        for job in jobs:
            events, previous = [], None
            rows = db.execute("SELECT * FROM job_events WHERE request_id=? ORDER BY rowid",
                              (job["request_id"],)) if has_events else []
            for row in rows:
                event = {"stage": row["stage"], "boot_id": row["boot_id"],
                         "at": datetime.fromtimestamp(row["wall_time"], timezone.utc).isoformat(),
                         "duration_ms": row["duration_ms"], "attempt": row["attempt"]}
                if previous and previous["boot_id"] == row["boot_id"]:
                    event["since_previous_ms"] = round((row["monotonic_ns"] - previous["monotonic_ns"]) / 1e6, 3)
                events.append(event)
                previous = row
            result.append({**dict(job), "events": events})
        return result


if __name__ == "__main__":
    root = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=root / "grok-gadgets/voice/jobs.sqlite3")
    parser.add_argument("--last", type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.last <= 50:
        parser.error("--last must be between 1 and 50")
    print(json.dumps(timelines(args.database, args.last), ensure_ascii=False, indent=2))
