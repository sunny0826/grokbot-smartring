"""Exercise actual HTTPS upload and local speech recognition; leave the transcript ready for the MCP mailbox."""
import argparse
import http.client
import io
import json
from pathlib import Path
import secrets
import ssl
import subprocess
import tempfile
import threading
import time
import wave

from voice_bridge.server import Server
from voice_bridge.store import Store
from voice_bridge.worker import WhisperTranscriber, Worker


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path, help="48 kHz mono 16-bit speech fixture, at most 30 seconds")
    parser.add_argument("--model", default="base")
    parser.add_argument("--expect", help="optional substring expected in transcription")
    args = parser.parse_args()
    with wave.open(str(args.wav)) as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (1, 2, 48000)
        pcm = source.readframes(source.getnframes())
    body = io.BytesIO()
    with wave.open(body, "wb") as target:
        target.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        target.writeframes(pcm)
    audio = body.getvalue()
    with tempfile.TemporaryDirectory(prefix="my-bot-speech-") as name:
        directory = Path(name)
        cert, key = directory / "cert.pem", directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        token, rid = secrets.token_hex(24), secrets.token_hex(16)
        credentials = directory / "credentials.json"
        credentials.write_text(json.dumps({"devices": {"speech-test": {"token": token}}}))
        credentials.chmod(0o600)
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(cert, key)
        client_context = ssl.create_default_context(cafile=str(cert))
        store = Store(directory / "data")
        server = Server(("127.0.0.1", 0), store, credentials, server_context)
        worker = Worker(store, WhisperTranscriber(args.model))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        worker.start()

        def request(method, data=None):
            connection = http.client.HTTPSConnection("localhost", server.server_port,
                                                      context=client_context, timeout=10)
            connection.request(method, "/v1/recordings/" + rid, data,
                               {"Authorization": "Bearer " + token, "X-Device-ID": "speech-test",
                                "Content-Type": "audio/wav"})
            response = connection.getresponse()
            status, result = response.status, json.loads(response.read())
            connection.close()
            return status, result

        try:
            status, _ = request("PUT", audio)
            assert status == 201
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                _, receipt = request("GET")
                if receipt["status"] not in ("stored", "transcribing"):
                    break
                time.sleep(0.2)
            assert receipt["status"] == "ready", receipt
            assert request("PUT", audio)[0] == 200
            with store.lock:
                transcript = store.db.execute("SELECT transcript FROM jobs WHERE request_id=?", (rid,)).fetchone()[0]
            assert transcript and (not args.expect or args.expect in transcript), transcript
            print(json.dumps({"https_upload": "passed", "duplicate_replay": "passed",
                              "local_transcript": transcript, "mailbox_status": "ready"}, ensure_ascii=False))
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
            worker.stop()
            store.close()


if __name__ == "__main__":
    main()
