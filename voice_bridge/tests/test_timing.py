import importlib.util
import json
from pathlib import Path

import pytest

from conftest import DEVICE, RID, wav
from test_store import CLAIM, ready
from voice_bridge.store import RequestError, Store

spec = importlib.util.spec_from_file_location("voice_timings", Path(__file__).parents[1] / "tools/voice_timings.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def test_timeline_survives_restart_and_omits_content(tmp_path):
    path = tmp_path / "jobs"
    store = Store(path)
    store.enable_activation()
    ready(store, text="private speech sentinel")
    store.reserve_notification(1000)
    store.finish_notification(RID, 1, "accepted", 1120)
    store.claim_request(RID, CLAIM)
    store.reply_request(RID, CLAIM, "私密回复")
    store.mark_read(RID, DEVICE)
    store.mark_read(RID, DEVICE)
    store.close()
    with pytest.raises(RequestError):
        store = Store(path)
        try:
            store.claim_request(RID, "b" * 32)
        finally:
            store.close()
    data = report.timelines(path / "jobs.sqlite3")
    events = data[0]["events"]
    assert [e["stage"] for e in events] == ["stored", "transcribing", "ready", "notify_sending",
        "notify_accepted", "claimed", "reply_received", "rendered", "replied", "read"]
    assert all(e.get("since_previous_ms", 0) >= 0 for e in events)
    assert next(e for e in events if e["stage"] == "rendered")["duration_ms"] >= 0
    assert "private speech sentinel" not in json.dumps(data)
    assert "私密回复" not in json.dumps(data, ensure_ascii=False)
    assert CLAIM not in json.dumps(data)


def test_rejected_reply_does_not_publish_success_events(storage):
    ready(storage)
    storage.claim_request(RID, CLAIM)
    storage.renderer = lambda _: (_ for _ in ()).throw(ValueError("private failure"))
    with pytest.raises(RequestError, match="render_unavailable"):
        storage.reply_request(RID, CLAIM, "回复")
    assert storage.get(RID, DEVICE)["status"] == "claimed"
    assert not storage.db.execute("SELECT 1 FROM job_events WHERE stage='replied'").fetchone()


def test_timings_are_bounded_and_retries_keep_distinct_boots(tmp_path):
    path = tmp_path / "jobs"
    store = Store(path)
    store.put(RID, DEVICE, wav(), upload_ms=25)
    for _ in range(70):
        store.update(RID, "failed", retryable=True)
        store.retry(RID, DEVICE)
    assert store.db.execute("SELECT COUNT(*) FROM job_events").fetchone()[0] == 64
    store.close()
    store = Store(path)
    store.claim()
    store.close()
    events = report.timelines(path / "jobs.sqlite3")[0]["events"]
    assert events[-1]["stage"] == "transcribing"
    assert "since_previous_ms" not in events[-1]
