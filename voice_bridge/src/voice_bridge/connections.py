"""Bound persistent connections while reserving threads for short requests."""
from dataclasses import dataclass
import socket
import threading
import time


def disconnect(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()


@dataclass
class Connection:
    device: str | None = None
    deadline: float = 0
    timer: threading.Timer | None = None


class Connections:
    idle_seconds = 3
    lifetime_seconds = 30
    max_requests = 64

    def __init__(self):
        self.lock = threading.Lock()
        self.sockets = {}
        self.closing = False

    def add(self, sock):
        with self.lock:
            if self.closing:
                disconnect(sock)
                return False
            self.sockets[sock] = Connection()
            return True

    def replace(self, raw, secure):
        with self.lock:
            self.sockets.pop(raw, None)
            if self.closing:
                disconnect(secure)
                return False
            self.sockets[secure] = Connection()
            return True

    def keep(self, sock, device, count):
        with self.lock:
            state = self.sockets.get(sock)
            if self.closing or state is None or count >= self.max_requests:
                return False
            if state.device is not None:
                return state.device == device and time.monotonic() < state.deadline
            persistent = [s for s in self.sockets.values() if s.device is not None]
            if len(persistent) >= 8 or sum(s.device == device for s in persistent) >= 2:
                return False
            state.device = device
            state.deadline = time.monotonic() + self.lifetime_seconds
            state.timer = threading.Timer(self.lifetime_seconds, disconnect, (sock,))
            state.timer.daemon = True
            state.timer.start()
            return True

    def remove(self, sock):
        with self.lock:
            state = self.sockets.pop(sock, None)
            if state and state.timer:
                state.timer.cancel()

    def begin_close(self):
        with self.lock:
            self.closing = True

    def close(self):
        with self.lock:
            for sock, state in self.sockets.items():
                if state.timer:
                    state.timer.cancel()
                disconnect(sock)
