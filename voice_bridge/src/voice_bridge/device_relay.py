"""Loopback WebSocket adapter for the existing gateway's device TCP protocol."""
import argparse
import asyncio
from collections import deque
from contextlib import asynccontextmanager, suppress
from dataclasses import dataclass
import hmac
from http import HTTPStatus
import json
import logging
import os
from pathlib import Path
import signal
import sys

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from .config import ConfigError, DEVICE_ID, authorized, read_credentials

MAX_INPUT = 16384
MAX_OUTPUT = 2048

# websockets debug logging includes headers and frames; never pass it a configured logger.
_WIRE_LOG = logging.Logger("voice_device_relay.wire", level=logging.CRITICAL + 1)
_WIRE_LOG.disabled = True
_WIRE_LOG.propagate = False


@dataclass(frozen=True)
class Limits:
    connections: int = 16
    frames_per_second: int = 20
    handshake: float = 3
    hello: float = 2
    idle: float = 12
    connect: float = 2
    send: float = 2
    close: float = 1

    def __post_init__(self):
        if any(value <= 0 for value in vars(self).values()):
            raise ValueError("relay_limits_must_be_positive")


class InvalidFrame(ValueError):
    pass


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InvalidFrame("duplicate_key")
        result[key] = value
    return result


def _constant(_value):
    raise InvalidFrame("invalid_number")


def frame(data):
    if (not isinstance(data, bytes) or len(data) > MAX_INPUT
            or not data.endswith(b"\n") or data.count(b"\n") != 1):
        raise InvalidFrame("binary_single_line_required")
    try:
        message = json.loads(data.decode("utf-8"), object_pairs_hook=_object,
                             parse_constant=_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise InvalidFrame("invalid_json") from None
    if not isinstance(message, dict):
        raise InvalidFrame("object_required")
    return message


class Relay:
    def __init__(self, credentials, *, gateway_port=8764, limits=None):
        if not 1 <= gateway_port <= 65535:
            raise ValueError("invalid_gateway_port")
        self.credentials = credentials
        self.gateway_port = gateway_port
        self.limits = limits or Limits()
        self.connections = set()

    def connection_factory(self, *args, **kwargs):
        relay = self

        class TrackedConnection(ServerConnection):
            rejected = False

            def connection_made(self, transport):
                if len(relay.connections) >= relay.limits.connections:
                    self.rejected = True
                    transport.abort()
                    return
                relay.connections.add(self)
                super().connection_made(transport)

            def connection_lost(self, exc):
                if not self.rejected:
                    relay.connections.discard(self)
                    super().connection_lost(exc)

            def data_received(self, data):
                if not self.rejected:
                    super().data_received(data)

            def eof_received(self):
                if not self.rejected:
                    return super().eof_received()

        return TrackedConnection(*args, **kwargs)

    def authenticate(self, connection, request):
        if request.path != "/v1/control":
            return connection.respond(HTTPStatus.NOT_FOUND, "not_found\n")
        headers = request.headers
        auth = headers.get_all("Authorization")
        devices = headers.get_all("X-Device-ID")
        if (len(auth) != 1 or len(devices) != 1
                or not DEVICE_ID.fullmatch(devices[0]) or not auth[0].startswith("Bearer ")
                or not 16 <= len(auth[0][7:]) <= 256):
            return connection.respond(HTTPStatus.UNAUTHORIZED, "unauthorized\n")
        try:
            valid = authorized(self.credentials, devices[0], auth[0][7:])
        except ConfigError:
            return connection.respond(HTTPStatus.SERVICE_UNAVAILABLE, "credentials_unavailable\n")
        if not valid:
            return connection.respond(HTTPStatus.UNAUTHORIZED, "unauthorized\n")
        connection.device_identity = (devices[0], auth[0][7:])

    @staticmethod
    def response_headers(connection, request, response):
        response.headers["Cache-Control"] = "no-store"
        return response

    def validate_hello(self, data, identity):
        message = frame(data)
        device_id, token = identity
        device = message.get("device")
        supplied = message.get("token")
        if (message.get("type") != "hello" or not isinstance(device, dict)
                or device.get("device_id") != device_id or not isinstance(supplied, str)
                or not hmac.compare_digest(supplied.encode(), token.encode())
                or not authorized(self.credentials, device_id, token)):
            raise InvalidFrame("hello_identity_mismatch")

    async def to_gateway(self, ws, writer, first):
        recent = deque()
        data = first
        while True:
            now = asyncio.get_running_loop().time()
            while recent and now - recent[0] >= 1:
                recent.popleft()
            if len(recent) >= self.limits.frames_per_second:
                raise InvalidFrame("frame_rate_exceeded")
            recent.append(now)
            writer.write(data)
            await asyncio.wait_for(writer.drain(), self.limits.send)
            data = await asyncio.wait_for(ws.recv(), self.limits.idle)
            if frame(data).get("type") == "hello":
                raise InvalidFrame("hello_already_received")

    async def from_gateway(self, ws, reader):
        while True:
            data = await asyncio.wait_for(reader.readline(), self.limits.idle)
            if not data:
                return
            if len(data) > MAX_OUTPUT or not data.endswith(b"\n"):
                raise InvalidFrame("invalid_gateway_frame")
            await asyncio.wait_for(ws.send(data), self.limits.send)

    async def handle(self, ws):
        writer = None
        tasks = []
        code, reason = 1000, "upstream_closed"
        try:
            first = await asyncio.wait_for(ws.recv(), self.limits.hello)
            self.validate_hello(first, ws.device_identity)
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", self.gateway_port, limit=MAX_OUTPUT),
                self.limits.connect)
            writer.transport.set_write_buffer_limits(high=MAX_INPUT * 2)
            tasks = [asyncio.create_task(self.to_gateway(ws, writer, first)),
                     asyncio.create_task(self.from_gateway(ws, reader)),
                     asyncio.create_task(ws.wait_closed())]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (InvalidFrame, ConfigError, ValueError):
            code, reason = 1008, "invalid_device_frame"
        except (TimeoutError, OSError):
            code, reason = 1011, "transport_unavailable"
        except ConnectionClosed:
            pass
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if writer is not None:
                writer.close()
                with suppress(OSError, TimeoutError):
                    await asyncio.wait_for(writer.wait_closed(), self.limits.close)
            with suppress(ConnectionClosed, OSError):
                await ws.close(code, reason)

    @asynccontextmanager
    async def listen(self, port=8770):
        if not 0 <= port <= 65535:
            raise ValueError("invalid_relay_port")
        async with serve(
            self.handle, "127.0.0.1", port,
            process_request=self.authenticate, process_response=self.response_headers,
            create_connection=self.connection_factory,
            compression=None, origins=[None], server_header=None,
            max_size=MAX_INPUT, max_queue=2, write_limit=MAX_OUTPUT * 2,
            open_timeout=self.limits.handshake, close_timeout=self.limits.close,
            ping_interval=20, ping_timeout=10, logger=_WIRE_LOG,
        ) as server:
            yield server


async def run(args):
    read_credentials(args.credentials)
    relay = Relay(args.credentials, gateway_port=args.gateway_port,
                  limits=Limits(connections=args.max_connections))
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, stop.set)
    try:
        async with relay.listen(args.port) as server:
            port = server.sockets[0].getsockname()[1]
            print(f"Device relay listening on 127.0.0.1:{port}; gateway loopback:{args.gateway_port}",
                  flush=True)
            await stop.wait()
    finally:
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.remove_signal_handler(signum)


def main():
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "grok-gadgets"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--gateway-port", type=int, default=8764)
    parser.add_argument("--credentials", type=Path, default=config / "credentials.json")
    parser.add_argument("--max-connections", type=int, default=16)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535 or not 1 <= args.gateway_port <= 65535 or args.max_connections < 1:
        parser.error("invalid port or connection limit")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except Exception:
        print("Device relay unavailable: check credentials and loopback listener configuration.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
