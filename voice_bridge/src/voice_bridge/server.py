"""Bounded HTTPS HTTP/1.1 receiver; authentication precedes reading audio."""
import json
import re
import socket
import ssl
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .connections import Connections
from .config import ConfigError, DEVICE_ID, authorized
from .store import RequestError

MAX_AUDIO = 2_880_044
ROUTE = re.compile(r"/v1/recordings/([0-9a-fA-F]{32})(/(?:retry|read|response)|/pages/[0-9]{1,3})?")


def read_exclusions(headers):
    values = headers.get_all("X-Voice-Read-Exclude", [])
    if not values:
        return ()
    if len(values) != 1 or len(values[0]) > 527:
        raise RequestError(400, "invalid_read_exclusions")
    ids = values[0].split(",")
    if len(ids) > 16 or any(not re.fullmatch(r"[0-9a-fA-F]{32}", rid) for rid in ids):
        raise RequestError(400, "invalid_read_exclusions")
    return tuple(dict.fromkeys(rid.lower() for rid in ids))


def validate_wav(audio):
    if len(audio) < 44 or len(audio) > MAX_AUDIO:
        raise RequestError(422, "invalid_wav")
    header = struct.unpack("<4sI4s4sIHHIIHH4sI", audio[:44])
    (riff, size, wave, fmt, fmt_size, codec, channels, rate,
     byte_rate, align, bits, data, data_size) = header
    if (riff != b"RIFF" or size != len(audio) - 8 or wave != b"WAVE" or fmt != b"fmt "
            or fmt_size != 16 or codec != 1 or channels != 1 or rate != 48000
            or byte_rate != 96000 or align != 2 or bits != 16 or data != b"data"
            or data_size != len(audio) - 44 or data_size % 2 or not 28800 <= data_size <= 2880000):
        raise RequestError(422, "invalid_wav")


class Server(ThreadingHTTPServer):
    daemon_threads = False
    block_on_close = True

    def __init__(self, address, store, credentials, context):
        self.store, self.credentials = store, credentials
        self.context = context
        self.slots = threading.BoundedSemaphore(16)
        self.connections = Connections()
        super().__init__(address, Handler)

    def get_request(self):
        # TLS handshake runs in the bounded request thread, not the accept loop.
        sock, address = self.socket.accept()
        sock.settimeout(15)
        return sock, address

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        if not self.connections.add(request):
            self.slots.release()
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.connections.remove(request)
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        secure = None
        try:
            secure = self.context.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
            if self.connections.replace(request, secure):
                secure.do_handshake()
                super().process_request_thread(secure, client_address)
        except (OSError, ssl.SSLError):
            pass
        finally:
            self.connections.remove(secure if secure is not None else request)
            (secure if secure is not None else request).close()
            self.slots.release()

    def server_close(self):
        self.connections.begin_close()
        self.store.stop_responses()
        # Give awakened response waiters a bounded chance to send their 503.
        deadline = time.monotonic() + 0.2
        for thread in list(self._threads) if isinstance(self._threads, list) else ():
            thread.join(max(0, deadline - time.monotonic()))
        self.connections.close()
        super().server_close()

    def handle_error(self, request, client_address):
        pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VoiceBridge"
    sys_version = ""

    def setup(self):
        super().setup()
        self.request_count = 0

    def handle_one_request(self):
        self.authenticated_device = None
        self.response_headers = {}
        self.request_complete = False
        self.request_count += 1
        if self.request_count > 1:
            self.connection.settimeout(self.server.connections.idle_seconds)
        super().handle_one_request()

    def log_message(self, *args):
        pass

    def send_error(self, code, message=None, explain=None):
        self.respond(code, {"error": "http_error"})

    def handle_expect_100(self):
        # ESP firmware sends its body directly. Reject rather than invite an
        # unauthenticated client to stream a large body before header validation.
        self.respond(417, {"error": "expect_not_supported"})
        return False

    def respond(self, status, body):
        binary = isinstance(body, bytes)
        data = body if binary else json.dumps(body, separators=(",", ":")).encode()
        connection_headers = self.headers.get_all("Connection", []) if hasattr(self, "headers") else []
        explicit_keep = len(connection_headers) == 1 and connection_headers[0].lower() == "keep-alive"
        keep = (200 <= status < 300 and self.request_complete and self.authenticated_device is not None
                and explicit_keep and self.server.connections.keep(
                    self.connection, self.authenticated_device, self.request_count))
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream" if binary else "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "keep-alive" if keep else "close")
        for name, value in self.response_headers.items():
            self.send_header(name, value)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.close_connection = not keep
        self.wfile.write(data)

    def dispatch(self):
        try:
            match = ROUTE.fullmatch(self.path)
            inbox = self.path == "/v1/inbox"
            if match is None and not inbox:
                raise RequestError(404, "not_found")
            request_id, action = match.groups() if match else ("", "/inbox")
            request_id = request_id.lower()
            device = self.headers.get("X-Device-ID", "")
            auth = self.headers.get("Authorization", "")
            if (len(self.headers.get_all("Authorization", [])) != 1
                    or len(self.headers.get_all("X-Device-ID", [])) != 1
                    or not DEVICE_ID.fullmatch(device) or not auth.startswith("Bearer ")
                    or not authorized(self.server.credentials, device, auth[7:])):
                raise RequestError(401, "unauthorized")
            self.authenticated_device = device
            if self.headers.get_all("Transfer-Encoding", []):
                raise RequestError(400, "transfer_encoding_not_supported")
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) > 1:
                raise RequestError(400, "invalid_content_length")
            raw_length = lengths[0] if lengths else "0"
            if not re.fullmatch(r"[0-9]{1,10}", raw_length):
                raise RequestError(400, "invalid_content_length")
            length = int(raw_length)
            if self.command == "PUT" and action is None:
                if not lengths:
                    raise RequestError(411, "content_length_required")
                if length > MAX_AUDIO:
                    raise RequestError(413, "audio_too_large")
                if length < 28844:
                    raise RequestError(422, "invalid_wav")
                if (len(self.headers.get_all("Content-Type", [])) != 1
                        or self.headers.get("Content-Type", "").lower() != "audio/wav"):
                    raise RequestError(415, "audio_wav_required")
                upload_started = time.monotonic_ns()
                audio = self.rfile.read(length)
                upload_ms = (time.monotonic_ns() - upload_started) / 1e6
                if len(audio) != length:
                    raise RequestError(400, "incomplete_body")
                validate_wav(audio)
                status, body = self.server.store.put(request_id, device, audio, upload_ms=upload_ms)
            elif length:
                raise RequestError(400, "body_not_allowed")
            elif self.command == "GET" and action is None:
                status, body = 200, self.server.store.get(request_id, device)
            elif self.command == "GET" and action == "/response":
                values = self.headers.get_all("X-Voice-After", [])
                if len(values) > 1:
                    raise RequestError(400, "invalid_voice_after")
                body, self.response_headers = self.server.store.response(
                    request_id, device, values[0] if values else None)
                status = 200
            elif self.command == "GET" and inbox:
                status, body = 200, self.server.store.inbox(device, read_exclusions(self.headers))
            elif self.command == "GET" and action and action.startswith("/pages/"):
                status, body = 200, self.server.store.page(request_id, device, int(action.rsplit("/", 1)[1]))
            elif self.command == "POST" and action == "/retry":
                status, body = 200, self.server.store.retry(request_id, device)
            elif self.command == "POST" and action == "/read":
                status, body = 200, self.server.store.mark_read(request_id, device)
            else:
                raise RequestError(405, "method_not_allowed")
            self.request_complete = True
            self.respond(status, body)
        except RequestError as error:
            self.respond(error.status, {"error": error.code})
        except ConfigError:
            self.respond(503, {"error": "credentials_unavailable"})
        except (TimeoutError, socket.timeout):
            self.respond(408, {"error": "request_timeout"})
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except Exception:
            self.respond(503, {"error": "service_unavailable"})

    do_PUT = dispatch
    do_GET = dispatch
    do_POST = dispatch
