"""Exercise exclusions through the real authenticated HTTPS Handler and SQLite."""
import http.client
import json
from types import SimpleNamespace

import pytest

from conftest import DEVICE, TOKEN, wav
from voice_bridge.store import RequestError
from voice_bridge.worker import Worker

HEADER = "X-Voice-Read-Exclude"
A, B, C = "a" * 32, "b" * 32, "c" * 32


def exchange(receiver, exclusions=(), *, device=DEVICE, token=TOKEN,
             method="GET", path="/v1/inbox"):
    server, context, _ = receiver
    connection = http.client.HTTPSConnection(
        "localhost", server.server_port, context=context, timeout=3)
    try:
        connection.putrequest(method, path)
        connection.putheader("Authorization", "Bearer " + token)
        connection.putheader("X-Device-ID", device)
        for value in exclusions:
            connection.putheader(HEADER, value)
        connection.endheaders()
        response = connection.getresponse()
        data = response.read()
        body = (data if response.getheader("Content-Type") == "application/octet-stream"
                else json.loads(data))
        return response.status, body
    finally:
        connection.close()


def replied(storage, request_id, device=DEVICE):
    storage.renderer = lambda _: SimpleNamespace(pages=[b"page"], warning=None)
    storage.put(request_id, device, wav())
    assert Worker(storage, lambda _: "test transcript").process_one()
    claim = f"{int(request_id, 16) ^ (1 << 127):032x}"
    storage.claim_request(request_id, claim)
    storage.reply_request(request_id, claim, "test reply")


def test_skip_a_then_b_without_marking_read(receiver, storage):
    for rid in (A, B, C):
        replied(storage, rid)
    assert exchange(receiver)[1]["request_id"] == A
    assert exchange(receiver, [A])[1]["request_id"] == B
    assert exchange(receiver, [A])[1]["request_id"] == B
    assert exchange(receiver, [A + "," + B])[1]["request_id"] == C
    assert exchange(receiver, [",".join((A, B, C))])[1]["status"] == "empty"
    assert exchange(receiver)[1]["request_id"] == A
    assert all(not storage.get(rid, DEVICE)["read"] for rid in (A, B, C))


def test_failed_read_does_not_block_next_inbox(receiver, storage, monkeypatch):
    for rid in (A, B, C):
        replied(storage, rid)
    def unavailable(request_id, device):
        assert request_id == A and device == DEVICE
        raise RequestError(503, "read_unavailable")
    monkeypatch.setattr(storage, "mark_read", unavailable)
    for _ in range(2):
        assert exchange(receiver, method="POST", path="/v1/recordings/" + A + "/read")[0] == 503
        assert exchange(receiver, [A])[1]["request_id"] == B
    assert exchange(receiver, [A + "," + B])[1]["request_id"] == C
    assert all(not storage.get(rid, DEVICE)["read"] for rid in (A, B, C))


@pytest.mark.parametrize("count", [0, 1, 16, 17])
def test_exclusion_capacity(receiver, storage, count):
    ids = [f"{i:032x}" for i in range(1, 19)]
    for rid in ids:
        replied(storage, rid)
    values = [",".join(ids[:count])] if count else []
    status, body = exchange(receiver, values)
    if count == 17:
        assert status == 400
    else:
        assert status == 200 and body["request_id"] == ids[count]
        assert not storage.get(ids[0], DEVICE)["read"]


def test_uppercase_and_repeated_ids(receiver, storage):
    replied(storage, A)
    replied(storage, B)
    assert exchange(receiver, [A.upper() + "," + A])[1]["request_id"] == B
    assert exchange(receiver, [",".join([A] * 16)])[1]["request_id"] == B
    assert exchange(receiver, [",".join([A] * 17)])[0] == 400


@pytest.mark.parametrize("values", [
    [""], ["g" * 32], ["a" * 31], ["a" * 33], [A + ","], ["," + A],
    [A + ",," + B], [A + ", " + B], ["a" * 528], [A, B], [A, A],
    [A + ";" + B], ["' OR 1=1 --"],
])
def test_malformed_headers_rejected(receiver, values):
    status, body = exchange(receiver, values)
    assert status == 400 and body["error"] == "invalid_read_exclusions"


def test_authentication_precedes_exclusion_validation(receiver):
    assert exchange(receiver, ["malformed"], token="invalid")[0] == 401


def test_device_status_read_and_rowid_filters(receiver, storage):
    replied(storage, C, "other-ring")
    storage.put("d" * 32, DEVICE, wav())
    assert Worker(storage, lambda _: "not answered").process_one()
    replied(storage, B)
    replied(storage, A)
    storage.mark_read(B, DEVICE)
    assert exchange(receiver)[1]["request_id"] == A
    assert exchange(receiver, [A])[1]["status"] == "empty"
    assert exchange(receiver, [A, A], device="other-ring", token=TOKEN + "other")[0] == 400
    assert exchange(receiver, [A], device="other-ring", token=TOKEN + "other")[1]["request_id"] == C
    assert exchange(receiver, [C])[1]["request_id"] == A


def test_rowid_order_and_non_inbox_routes_ignore_header(receiver, storage):
    replied(storage, B)
    replied(storage, A)
    assert exchange(receiver)[1]["request_id"] == B
    assert exchange(receiver, [B])[1]["request_id"] == A
    base = "/v1/recordings/" + B
    assert exchange(receiver, ["invalid"], path=base)[1]["request_id"] == B
    assert exchange(receiver, ["invalid"], path=base + "/pages/0") == (200, b"page")
    for _ in range(2):
        assert exchange(receiver, ["invalid"], method="POST", path=base + "/read")[1]["read"]
    assert exchange(receiver, ["invalid"], method="POST")[0] == 405
    assert exchange(receiver)[1]["request_id"] == A
