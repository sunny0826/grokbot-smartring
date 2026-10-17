from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import socket

import pytest

from conftest import DEVICE, RID, TOKEN, private_write, wav
from voice_bridge.config import ConfigError, read_credentials
from voice_bridge.server import MAX_AUDIO
from voice_bridge.store import RequestError, Store


def request(receiver, method="PUT", request_id=RID, body=None, headers=None, suffix="", path=None):
    server, context, _ = receiver
    connection = http.client.HTTPSConnection("localhost", server.server_port, context=context, timeout=3)
    fields = {"Authorization": "Bearer " + TOKEN, "X-Device-ID": DEVICE}
    if method == "PUT":
        body = wav() if body is None else body
        fields["Content-Type"] = "audio/wav"
    fields.update(headers or {})
    connection.request(method, path or "/v1/recordings/" + request_id + suffix, body, fields)
    response = connection.getresponse()
    assert response.getheader("Cache-Control") == "no-store"
    data = response.read()
    result = response.status, data if response.getheader("Content-Type") == "application/octet-stream" else json.loads(data)
    connection.close()
    return result


def test_tls_durable_upload_idempotency_and_conflict(receiver, storage):
    assert request(receiver) == (201, {"request_id": RID, "status": "stored", "page_count": 0, "read": False})
    assert request(receiver)[0] == 200
    assert request(receiver, body=wav(sample=1))[0] == 409
    assert request(receiver, "GET")[1]["status"] == "stored"
    assert bytes(storage.db.execute("SELECT audio FROM jobs").fetchone()[0]) == wav()
    assert request(receiver, "GET", headers={"X-Device-ID": "other-ring",
                   "Authorization": "Bearer " + TOKEN + "other"})[0] == 404
    assert request(receiver, headers={"X-Device-ID": "other-ring",
                   "Authorization": "Bearer " + TOKEN + "other"})[0] == 409


def test_auth_revocation_and_limits_before_body(receiver):
    server, context, _ = receiver
    def headers_only(overrides):
        connection = http.client.HTTPSConnection("localhost", server.server_port, context=context, timeout=3)
        fields = {"X-Device-ID": DEVICE, "Authorization": "Bearer " + TOKEN,
                  "Content-Type": "audio/wav", "Content-Length": str(len(wav()))}
        fields.update(overrides)
        connection.putrequest("PUT", "/v1/recordings/" + RID)
        for name, value in fields.items():
            connection.putheader(name, value)
        connection.endheaders()
        response = connection.getresponse()
        response.read()
        connection.close()
        return response.status
    assert headers_only({"Authorization": "Bearer wrong"}) == 401
    assert headers_only({"Content-Length": str(MAX_AUDIO + 1)}) == 413
    assert request(receiver, body=b"notwav")[0] == 422
    assert headers_only({"Content-Type": "application/octet-stream"}) == 415
    assert request(receiver, request_id="../bad")[0] == 404
    private_write(receiver[2], {"devices": {DEVICE: {"token": TOKEN, "revoked": True}}})
    assert headers_only({}) == 401


def test_unauthorized_does_not_wait_for_body(receiver):
    server, context, _ = receiver
    with context.wrap_socket(socket.create_connection(("127.0.0.1", server.server_port)),
                             server_hostname="localhost") as stream:
        stream.settimeout(2)
        stream.sendall((f"PUT /v1/recordings/{RID} HTTP/1.1\r\nHost: localhost\r\n"
                        f"X-Device-ID: {DEVICE}\r\nAuthorization: Bearer bad\r\n"
                        "Content-Type: audio/wav\r\nContent-Length: 2880044\r\n\r\n").encode())
        assert b"401" in stream.recv(1024)


@pytest.mark.parametrize("body", [wav(0.299), wav() + b"00",
                                  b"NOPE" + wav()[4:], wav()[:20] + b"\x03\x00" + wav()[22:]],
                         ids=["too-short", "trailing-bytes", "bad-magic", "non-pcm"])
def test_invalid_wav(receiver, body):
    assert request(receiver, body=body)[0] in (413, 422)


def test_maximum_duration(receiver):
    assert len(wav(30)) == MAX_AUDIO
    assert request(receiver, body=wav(30))[0] == 201


def test_parallel_put_one_job(receiver, storage):
    with ThreadPoolExecutor(max_workers=6) as executor:
        results = list(executor.map(lambda _: request(receiver)[0], range(6)))
    assert results.count(201) == 1 and results.count(200) == 5
    assert storage.db.execute("SELECT COUNT(*) FROM jobs").fetchone()[0] == 1














def test_storage_full_keeps_pending_and_duplicate(tmp_path):
    store = Store(tmp_path / "bounded", max_jobs=1, max_bytes=len(wav()))
    store.put(RID, DEVICE, wav())
    assert store.put(RID, DEVICE, wav())[0] == 200
    with pytest.raises(RequestError, match="storage_full"):
        store.put("f" * 32, DEVICE, wav())
    assert store.get(RID, DEVICE)["status"] == "stored"
    store.close()


def test_credentials_strict_parser(tmp_path):
    path = tmp_path / "credentials.json"
    for content in ['{"devices":{},"devices":{}}', '{"devices":{"x":{"token":"short"}}}',
                    '{"devices":{"x":{"token":"0123456789abcdef","revoked":1}}}',
                    '{"devices":{},"bad":NaN}']:
        path.write_text(content)
        path.chmod(0o600)
        with pytest.raises(ConfigError):
            read_credentials(path)
    private_write(path, {"devices": {DEVICE: {"token": TOKEN}}})
    assert DEVICE in read_credentials(path)
    path.chmod(0o644)
    with pytest.raises(ConfigError):
        read_credentials(path)
