from concurrent.futures import ThreadPoolExecutor
import json

import pytest

from conftest import DEVICE, RID, private_write, wav
from voice_bridge.config import ConfigError
from voice_bridge.notifier import Notifier
from voice_bridge.store import Store, RequestError
from voice_bridge.trigger import Delivery, TriggerConfig, configure
from voice_bridge.worker import Worker


def ready(store, rid=RID):
    store.put(rid, DEVICE, wav())
    Worker(store, lambda _: "自动回复测试").process_one()


def test_activation_is_opt_in_and_old_ready_requires_explicit_opt_in(storage):
    ready(storage)
    storage.enable_activation()
    assert storage.reserve_notification(1000) is None
    storage.enable_activation(include_existing=True)
    assert storage.reserve_notification(1000) == {"request_id": RID, "attempt": 1}


def test_ready_notification_survives_restart_and_unknown_delivery(tmp_path):
    directory = tmp_path / "data"
    store = Store(directory)
    store.enable_activation()
    ready(store)
    store.close()
    store = Store(directory)
    store.enable_activation()
    assert store.reserve_notification(1000)["attempt"] == 1
    store.close()
    store = Store(directory)
    try:
        store.enable_activation()
        assert store.get(RID, DEVICE)["activation"]["status"] == "uncertain"
        assert store.reserve_notification(1119) is None
        assert store.reserve_notification(1120)["attempt"] == 2
    finally:
        store.close()


@pytest.mark.parametrize("delivery", [Delivery("accepted"), Delivery("uncertain", "trigger_transport_unknown")])
def test_accepted_or_unknown_waits_then_rechecks_ready(storage, delivery):
    storage.enable_activation()
    ready(storage)
    now = [1000]
    calls = []
    notifier = Notifier(storage, lambda rid: calls.append(rid) or delivery, clock=lambda: now[0])
    assert notifier.process_one()
    assert storage.get(RID, DEVICE)["status"] == "ready"
    now[0] = 1119
    assert not notifier.process_one()
    now[0] = 1126
    assert notifier.process_one()
    assert calls == [RID, RID]
    storage.claim_request(RID, "f" * 32)
    now[0] = 10000
    assert not notifier.process_one()
    assert storage.get(RID, DEVICE)["status"] == "claimed"


def test_slow_send_cannot_overwrite_a_claim(storage):
    storage.enable_activation()
    ready(storage)
    def send(_):
        storage.claim_request(RID, "f" * 32)
        return Delivery("accepted")
    assert Notifier(storage, send, clock=lambda: 1000).process_one()
    assert storage.get(RID, DEVICE)["status"] == "claimed"
    assert storage.reserve_notification(10000) is None


def test_retry_after_and_global_spacing_survive_restart(tmp_path):
    directory = tmp_path / "data"
    store = Store(directory)
    store.enable_activation()
    ready(store)
    ready(store, "b" * 32)
    notifier = Notifier(store, lambda _: Delivery("retry", "trigger_http_429", 300), clock=lambda: 1000)
    assert notifier.process_one()
    store.close()
    store = Store(directory)
    try:
        store.enable_activation()
        assert store.reserve_notification(1014) is None
        assert store.reserve_notification(1015) is None
        assert store.reserve_notification(1299) is None
        assert store.reserve_notification(1306)["request_id"] == "b" * 32
        assert store.reserve_notification(1321)["request_id"] == RID
    finally:
        store.close()


def test_auth_failure_pauses_all_notifications_until_explicit_recovery(storage):
    storage.enable_activation()
    ready(storage)
    ready(storage, "b" * 32)
    notifier = Notifier(storage, lambda _: Delivery("blocked", "trigger_http_401", pause=True), clock=lambda: 1000)
    assert notifier.process_one()
    assert storage.activation_state()["error"] == "trigger_http_401"
    assert storage.reserve_notification(10000) is None
    assert storage.get(RID, DEVICE)["status"] == "ready"
    storage.enable_activation(retry_blocked=True)
    assert storage.reserve_notification(10000)["request_id"] == RID


def test_auth_pause_survives_restart(tmp_path):
    directory = tmp_path / "data"
    store = Store(directory)
    store.enable_activation()
    ready(store)
    ready(store, "b" * 32)
    store.reserve_notification(1000)
    store.finish_notification(RID, 1, "blocked", 1000, "trigger_http_401", pause=True)
    store.close()
    store = Store(directory)
    try:
        store.enable_activation()
        assert store.activation_state()["error"] == "trigger_http_401"
        assert store.reserve_notification(10000) is None
        store.enable_activation(retry_blocked=True)
        assert store.reserve_notification(10000)["request_id"] == RID
    finally:
        store.close()


def test_retry_budget_does_not_starve_new_requests_or_reset_on_restart(tmp_path):
    directory = tmp_path / "data"
    store = Store(directory)
    store.enable_activation()
    ready(store)
    for attempt in range(1, 7):
        row = store.reserve_notification(attempt * 1000)
        assert row["attempt"] == attempt
        store.finish_notification(RID, attempt, "retry", attempt * 1000 + 30, "trigger_http_503")
    store.close()
    store = Store(directory)
    try:
        store.enable_activation()
        ready(store, "b" * 32)
        assert store.reserve_notification(7000)["request_id"] == "b" * 32
        assert store.get(RID, DEVICE)["activation"]["error"] == "trigger_attempts_exhausted"
        assert store.get(RID, DEVICE)["status"] == "ready"
    finally:
        store.close()


def test_duplicate_runs_only_one_claim_can_execute(storage):
    storage.enable_activation()
    ready(storage)
    def claim(value):
        try:
            return storage.claim_request(RID, value * 32)["duplicate"]
        except RequestError as error:
            return error.code
    with ThreadPoolExecutor(2) as pool:
        results = list(pool.map(claim, ["c", "d"]))
    assert results.count(False) == 1
    assert results.count("claim_conflict") == 1
    assert storage.reserve_notification(1000) is None


def test_trigger_configuration_is_private_and_errors_are_redacted(tmp_path):
    path = tmp_path / "trigger.json"
    secret = "test-secret-for-local-tests"
    private_write(path, {"url": "https://example.com/hook", "key": secret})
    config = TriggerConfig.read(path)
    assert secret not in repr(config) and "example.com" not in repr(config)
    path.chmod(0o644)
    with pytest.raises(ConfigError):
        TriggerConfig.read(path)
    for url in ["http://example.com/hook", "https://user:secret@example.com/hook",
                "https://example.com/hook?secret=abc", "https://example.com/hook#fragment",
                "https://example.com:broken/hook", "https://example.com/hook\r\nInjected:x"]:
        with pytest.raises(ConfigError) as error:
            TriggerConfig.parse({"url": url, "key": secret})
        assert url not in str(error.value) and secret not in str(error.value)


def test_legacy_migration_does_not_activate_historical_audio(tmp_path):
    store = Store(tmp_path / "data")
    store.put(RID, DEVICE, wav())
    store.update(RID, "needs_configuration", transcript="历史录音")
    store.close()
    store = Store(tmp_path / "data")
    try:
        store.enable_activation()
        assert store.get(RID, DEVICE)["status"] == "ready"
        assert store.reserve_notification(1000) is None
    finally:
        store.close()


def test_setup_without_a_terminal_never_reads_or_echoes_secrets(tmp_path, monkeypatch):
    import io
    monkeypatch.setattr("sys.stdin", io.StringIO("sensitive-input\n"))
    with pytest.raises(ConfigError, match="requires_terminal"):
        configure(tmp_path / "trigger.json")
    assert not (tmp_path / "trigger.json").exists()


def test_generated_routine_targets_the_configured_mailbox(monkeypatch, capsys):
    from voice_bridge.cli import main
    monkeypatch.setattr("sys.argv", ["voice-bridge", "--mailbox-id", "custom-mailbox", "--print-grok-routine"])
    assert main() == 0
    output = capsys.readouterr().out
    assert "mailbox_id=custom-mailbox" in output
    assert "mailbox_id=smartring-voice-mailbox" not in output
    monkeypatch.setattr("sys.argv", ["voice-bridge", "--mailbox-id", "bad\ninstruction", "--print-grok-routine"])
    with pytest.raises(SystemExit) as error:
        main()
    assert error.value.code == 2
