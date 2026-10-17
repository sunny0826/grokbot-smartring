import json
import os
from pathlib import Path
import ssl
import struct
import subprocess
import threading

import pytest

from voice_bridge.server import Server
from voice_bridge.store import Store

TOKEN = "test-only-device-token-123456"
DEVICE = "test-ring"
RID = "0123456789abcdef0123456789abcdef"


def private_write(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def wav(seconds=0.3, sample=0):
    size = int(48000 * seconds) * 2
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", size + 36, b"WAVE", b"fmt ",
                       16, 1, 1, 48000, 96000, 2, 16, b"data", size) + bytes([sample]) * size


@pytest.fixture
def storage(tmp_path):
    store = Store(tmp_path / "data")
    yield store
    store.close()




@pytest.fixture(scope="session")
def tls(tmp_path_factory):
    directory = tmp_path_factory.mktemp("tls")
    cert, key = directory / "cert.pem", directory / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.load_cert_chain(cert, key)
    client = ssl.create_default_context(cafile=str(cert))
    return server, client


@pytest.fixture
def receiver(tmp_path, storage, tls):
    credentials = tmp_path / "credentials.json"
    private_write(credentials, {"devices": {DEVICE: {"token": TOKEN},
                                            "other-ring": {"token": TOKEN + "other"}}})
    server = Server(("127.0.0.1", 0), storage, credentials, tls[0])
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server, tls[1], credentials
    server.shutdown()
    thread.join()
    server.server_close()
