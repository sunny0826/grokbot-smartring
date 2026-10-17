"""Grok Bot routine webhook transport; credentials stay in a private file."""
from dataclasses import dataclass, field
from email.utils import parsedate_to_datetime
import getpass
import http.client
import json
import os
from pathlib import Path
import ssl
import sys
import tempfile
import time
from urllib.parse import urlsplit

from .config import ConfigError, private_json


@dataclass(frozen=True)
class TriggerConfig:
    url: str = field(repr=False)
    key: str = field(repr=False)

    @classmethod
    def parse(cls, value):
        try:
            if set(value) != {"url", "key"}:
                raise ValueError()
            url, key = value["url"], value["key"]
            if (not isinstance(url, str) or not isinstance(key, str)
                    or not 16 <= len(key) <= 4096 or len(url) > 4096
                    or any(ord(c) < 33 or ord(c) > 126 for c in key + url)):
                raise ValueError()
            parsed = urlsplit(url)
            if (parsed.scheme != "https" or not parsed.hostname or not parsed.path
                    or parsed.username or parsed.password or parsed.query or parsed.fragment
                    or (parsed.port is not None and not 1 <= parsed.port <= 65535)):
                raise ValueError()
            return cls(url, key)
        except (TypeError, ValueError):
            raise ConfigError("invalid_grok_trigger_configuration") from None

    @classmethod
    def read(cls, path):
        return cls.parse(private_json(path))


def configure(path):
    if not sys.stdin.isatty():
        raise ConfigError("trigger_configuration_requires_terminal")
    path = Path(path).expanduser()
    url = getpass.getpass("Grok Bot routine POST URL (hidden): ").strip()
    key = getpass.getpass("Grok Bot routine key (hidden): ").strip()
    config = TriggerConfig.parse({"url": url, "key": key})
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.parent.stat().st_mode & 0o077:
        raise ConfigError("trigger_directory_requires_mode_0700")
    name = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as handle:
            name = handle.name
            os.chmod(name, 0o600)
            json.dump({"url": config.url, "key": config.key}, handle)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(name, path)
    finally:
        if name and os.path.exists(name):
            os.unlink(name)


@dataclass(frozen=True)
class Delivery:
    state: str
    error: str | None = None
    retry_after: float = 0
    pause: bool = False


def retry_after(value):
    try:
        seconds = float(value)
    except (TypeError, ValueError):
        try:
            seconds = parsedate_to_datetime(value).timestamp() - time.time()
        except (TypeError, ValueError, OverflowError):
            return 0
    return max(0, min(3600, seconds))


class TriggerSender:
    def __init__(self, config, mailbox_id, *, context=None, transcript_store=None):
        self.config, self.mailbox_id = config, mailbox_id
        self.transcript_store = transcript_store
        self.context = context or ssl.create_default_context()

    def __call__(self, request_id):
        target = urlsplit(self.config.url)
        connection = http.client.HTTPSConnection(target.hostname, target.port or 443,
                                                 context=self.context, timeout=10)
        payload = {"event": "voice.ready", "version": 1,
                   "request_id": request_id, "mailbox_id": self.mailbox_id}
        if self.transcript_store is not None:
            payload.update(self.transcript_store.trigger_transcript(request_id))
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        try:
            connection.request("POST", target.path, body, {
                "Authorization": "Bearer " + self.config.key,
                "Content-Type": "application/json", "User-Agent": "smartring-voice-bridge/0.3.1"})
            response = connection.getresponse()
            if response.status == 200:
                return Delivery("accepted")
            if response.status == 429 or 500 <= response.status <= 599:
                return Delivery("retry", f"trigger_http_{response.status}",
                                retry_after(response.getheader("Retry-After")))
            return Delivery("blocked", f"trigger_http_{response.status}", pause=True)
        except (OSError, http.client.HTTPException):
            return Delivery("uncertain", "trigger_transport_unknown")
        finally:
            connection.close()
