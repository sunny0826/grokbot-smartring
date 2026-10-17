"""Durable recording and mailbox state, owned by one local process."""
import fcntl
import hashlib
import json
import os
import re
import sqlite3
import threading
import unicodedata
from pathlib import Path

ID = re.compile(r"[0-9a-f]{32}")


class RequestError(Exception):
    def __init__(self, status, code):
        self.status, self.code = status, code
        super().__init__(code)


class Store:
    def __init__(self, directory, max_jobs=1000, max_bytes=512 * 1024 * 1024, *, renderer=None):
        directory = Path(directory)
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if directory.stat().st_mode & 0o077:
            raise ValueError("storage_requires_mode_0700")
        self.lockfile = (directory / "process.lock").open("a+b")
        os.chmod(directory / "process.lock", 0o600)
        try:
            fcntl.flock(self.lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lockfile.close()
            raise ValueError("storage_already_in_use") from None
        self.lock = threading.RLock()
        self.max_jobs, self.max_bytes, self.renderer = max_jobs, max_bytes, renderer
        self.db = sqlite3.connect(directory / "jobs.sqlite3", check_same_thread=False)
        os.chmod(directory / "jobs.sqlite3", 0o600)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA fullfsync=ON")
        self.db.execute("""CREATE TABLE IF NOT EXISTS jobs (
            request_id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
            digest TEXT NOT NULL, audio BLOB NOT NULL, status TEXT NOT NULL,
            transcript TEXT, error TEXT, retryable INTEGER NOT NULL DEFAULT 0)""")
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(jobs)")}
        with self.db:
            for name, definition in {"claim_id": "TEXT", "claim_result": "TEXT", "reply": "TEXT",
                                     "reply_result": "TEXT", "read": "INTEGER NOT NULL DEFAULT 0",
                                     "page_count": "INTEGER NOT NULL DEFAULT 0"}.items():
                if name not in columns:
                    self.db.execute(f"ALTER TABLE jobs ADD COLUMN {name} {definition}")
            self.db.execute("CREATE UNIQUE INDEX IF NOT EXISTS unique_claim ON jobs(claim_id)")
            self.db.execute("""CREATE TABLE IF NOT EXISTS pages (
                request_id TEXT NOT NULL, page_index INTEGER NOT NULL, data BLOB NOT NULL,
                PRIMARY KEY(request_id,page_index))""")
            self.db.execute("UPDATE jobs SET status='stored' WHERE status='transcribing'")
            self.db.execute("""UPDATE jobs SET status='ready',error=NULL,retryable=0
                WHERE status='needs_configuration' AND transcript IS NOT NULL""")
            self.db.execute("""UPDATE jobs SET status='needs_review',error='legacy_delivery_needs_review',retryable=0
                WHERE status IN ('submitted','dispatching','unconfirmed','needs_configuration')""")

    def close(self):
        self.db.close()
        self.lockfile.close()

    @staticmethod
    def public(row):
        result = {"request_id": row["request_id"], "status": row["status"],
                  "page_count": row["page_count"], "read": bool(row["read"])}
        if row["error"]:
            result["error"] = row["error"]
        return result

    @staticmethod
    def _id(value):
        if not isinstance(value, str) or not ID.fullmatch(value):
            raise RequestError(400, "invalid_id")

    def _row(self, request_id):
        self._id(request_id)
        row = self.db.execute("SELECT * FROM jobs WHERE request_id=?", (request_id,)).fetchone()
        if row is None:
            raise RequestError(404, "not_found")
        return row

    def _owned(self, request_id, device_id):
        row = self._row(request_id)
        if row["device_id"] != device_id:
            raise RequestError(404, "not_found")
        return row

    def get(self, request_id, device_id):
        with self.lock:
            return self.public(self._owned(request_id, device_id))

    def _used(self):
        return self.db.execute("""SELECT (SELECT COALESCE(SUM(length(audio)),0) FROM jobs)
            + (SELECT COALESCE(SUM(length(data)),0) FROM pages)""").fetchone()[0]

    def put(self, request_id, device_id, audio):
        self._id(request_id)
        digest = hashlib.sha256(audio).hexdigest()
        with self.lock, self.db:
            row = self.db.execute("SELECT * FROM jobs WHERE request_id=?", (request_id,)).fetchone()
            if row:
                if row["device_id"] != device_id or row["digest"] != digest:
                    raise RequestError(409, "request_id_conflict")
                return 200, self.public(row)
            count = self.db.execute("SELECT COUNT(*) FROM jobs").fetchone()[0]
            if count >= self.max_jobs or self._used() + len(audio) > self.max_bytes:
                raise RequestError(507, "storage_full")
            self.db.execute("INSERT INTO jobs(request_id,device_id,digest,audio,status) VALUES(?,?,?,?,'stored')",
                            (request_id, device_id, digest, audio))
            return 201, self.public(self._row(request_id))

    def retry(self, request_id, device_id):
        with self.lock, self.db:
            row = self._owned(request_id, device_id)
            if not (row["status"] == "failed" and row["retryable"]):
                raise RequestError(409, "retry_not_allowed")
            self.db.execute("UPDATE jobs SET status='stored',error=NULL,retryable=0 WHERE request_id=?",
                            (request_id,))
            return self.public(self._row(request_id))

    def claim(self):
        with self.lock, self.db:
            row = self.db.execute("SELECT * FROM jobs WHERE status='stored' ORDER BY rowid LIMIT 1").fetchone()
            if row:
                self.db.execute("UPDATE jobs SET status='transcribing' WHERE request_id=?", (row["request_id"],))
            return dict(row) if row else None

    def update(self, request_id, status, *, transcript=None, error=None, retryable=False):
        with self.lock, self.db:
            self.db.execute("""UPDATE jobs SET status=?, transcript=COALESCE(?,transcript),
                error=?,retryable=? WHERE request_id=?""",
                (status, transcript, error, int(retryable), request_id))

    def mailbox_state(self):
        with self.lock:
            pending = self.db.execute("SELECT COUNT(*) FROM jobs WHERE status IN ('ready','claimed')").fetchone()[0]
            unread = self.db.execute("SELECT COUNT(*) FROM jobs WHERE status='replied' AND read=0").fetchone()[0]
            rows = self.db.execute("""SELECT request_id,device_id,status,transcript FROM jobs
                WHERE status IN ('ready','claimed','needs_review') OR (status='replied' AND read=0)
                ORDER BY CASE status WHEN 'ready' THEN 0 WHEN 'claimed' THEN 1
                WHEN 'needs_review' THEN 2 ELSE 3 END,rowid LIMIT 5""").fetchall()
            return {"pending_count": pending, "unread_count": unread, "requests": [
                {"request_id": r["request_id"], "device_id": r["device_id"], "status": r["status"],
                 "preview": (r["transcript"] or "")[:24]} for r in rows]}

    @staticmethod
    def _transcript_page(row, offset):
        text = row["transcript"] or ""
        if type(offset) is not int or not 0 <= offset <= len(text):
            raise RequestError(400, "invalid_offset")
        end = min(offset + 800, len(text))
        return {"request_id": row["request_id"], "device_id": row["device_id"], "status": row["status"],
                "transcript": text[offset:end], "offset": offset,
                "next_offset": end if end < len(text) else None}

    @staticmethod
    def _reply_state(row):
        result = {"page_count": row["page_count"], "read": bool(row["read"])}
        if row["reply"] is not None:
            result["reply"] = row["reply"]
        return result

    def read_request(self, request_id, offset=0):
        with self.lock:
            row = self._row(request_id)
            return {**self._transcript_page(row, offset), **self._reply_state(row)}

    def claim_request(self, request_id, claim_id):
        self._id(claim_id)
        with self.lock, self.db:
            row = self._row(request_id)
            if row["claim_id"] == claim_id:
                return {**json.loads(row["claim_result"]), "status": row["status"],
                        "duplicate": True, **self._reply_state(row)}
            if row["status"] != "ready":
                raise RequestError(409, "claim_conflict")
            if self.db.execute("SELECT 1 FROM jobs WHERE claim_id=?", (claim_id,)).fetchone():
                raise RequestError(409, "claim_id_reused")
            result = {**self._transcript_page(row, 0), "status": "claimed", "claim_id": claim_id,
                      "duplicate": False, **self._reply_state(row)}
            self.db.execute("UPDATE jobs SET status='claimed',claim_id=?,claim_result=? WHERE request_id=?",
                            (claim_id, json.dumps(result), request_id))
            return result

    def reply_request(self, request_id, claim_id, text):
        self._id(claim_id)
        if (not isinstance(text, str) or not text.strip() or len(text) > 128
                or any(unicodedata.category(c) in ('Cc', 'Cs') and c != '\n' for c in text)):
            raise RequestError(400, "invalid_reply_text")
        with self.lock, self.db:
            row = self._row(request_id)
            if row["claim_id"] != claim_id:
                raise RequestError(409, "claim_conflict")
            if row["reply"] is not None:
                if row["reply"] != text:
                    raise RequestError(409, "reply_conflict")
                return json.loads(row["reply_result"])
            if row["status"] != "claimed":
                raise RequestError(409, "reply_not_allowed")
            if self.renderer is None:
                from .assets import font_path
                from .render import RenderPages
                try:
                    self.renderer = RenderPages(font_path())
                except Exception:
                    raise RequestError(503, "render_unavailable") from None
            try:
                rendered = self.renderer(text)
            except RequestError:
                raise
            except Exception:
                raise RequestError(503, "render_unavailable") from None
            if self._used() + sum(map(len, rendered.pages)) > self.max_bytes:
                raise RequestError(507, "storage_full")
            result = {"request_id": request_id, "status": "replied", "page_count": len(rendered.pages)}
            if rendered.warning:
                result["warning"] = rendered.warning
            self.db.executemany("INSERT INTO pages VALUES(?,?,?)",
                                [(request_id, i, p) for i, p in enumerate(rendered.pages)])
            self.db.execute("UPDATE jobs SET status='replied',reply=?,reply_result=?,page_count=? WHERE request_id=?",
                            (text, json.dumps(result), len(rendered.pages), request_id))
            return result

    def inbox(self, device_id):
        with self.lock:
            row = self.db.execute("""SELECT * FROM jobs WHERE device_id=? AND status='replied'
                AND read=0 ORDER BY rowid LIMIT 1""", (device_id,)).fetchone()
            return self.public(row) if row else {"request_id": "", "status": "empty", "page_count": 0, "read": False}

    def page(self, request_id, device_id, index):
        with self.lock:
            row = self._owned(request_id, device_id)
            if type(index) is not int or not 0 <= index < row["page_count"]:
                raise RequestError(404, "page_not_found")
            return bytes(self.db.execute("SELECT data FROM pages WHERE request_id=? AND page_index=?",
                                         (request_id, index)).fetchone()[0])

    def mark_read(self, request_id, device_id):
        with self.lock, self.db:
            row = self._owned(request_id, device_id)
            if row["status"] != "replied":
                raise RequestError(409, "reply_not_available")
            self.db.execute("UPDATE jobs SET read=1 WHERE request_id=?", (request_id,))
            return self.public(self._row(request_id))
