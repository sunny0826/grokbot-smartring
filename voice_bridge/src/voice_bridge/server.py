"""Bounded HTTPS HTTP/1.1 receiver; authentication precedes reading audio."""
import json
import re
import socket
import ssl
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .config import ConfigError, DEVICE_ID, authorized
from .store import RequestError

MAX_AUDIO = 2_880_044
ROUTE = re.compile(r"/v1/recordings/([0-9a-fA-F]{32})(/(?:retry|read)|/pages/[0-9]{1,3})?")


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
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            with self.context.wrap_socket(request, server_side=True) as secure:
                super().process_request_thread(secure, client_address)
        except (OSError, ssl.SSLError):
            request.close()
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VoiceBridge"
    sys_version = ""

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
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream" if binary else "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.close_connection = True
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
            if self.headers.get("Transfer-Encoding"):
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
                audio = self.rfile.read(length)
                if len(audio) != length:
                    raise RequestError(400, "incomplete_body")
                validate_wav(audio)
                status, body = self.server.store.put(request_id, device, audio)
            elif length:
                raise RequestError(400, "body_not_allowed")
            elif self.command == "GET" and action is None:
                status, body = 200, self.server.store.get(request_id, device)
            elif self.command == "GET" and inbox:
                status, body = 200, self.server.store.inbox(device)
            elif self.command == "GET" and action and action.startswith("/pages/"):
                status, body = 200, self.server.store.page(request_id, device, int(action.rsplit("/", 1)[1]))
            elif self.command == "POST" and action == "/retry":
                status, body = 200, self.server.store.retry(request_id, device)
            elif self.command == "POST" and action == "/read":
                status, body = 200, self.server.store.mark_read(request_id, device)
            else:
                raise RequestError(405, "method_not_allowed")
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
