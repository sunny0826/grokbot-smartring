"""Explicit, idempotent enrollment of the local software mailbox."""
import fcntl
import json
import os
from pathlib import Path
import secrets
import tempfile

from .config import ConfigError, DEVICE_ID, private_json, read_credentials
from .mailbox import MAILBOX_ID

PURPOSE = "my-bot-voice-mailbox-v1"


def enroll(credentials, device_id=MAILBOX_ID):
    path = Path(credentials)
    if not DEVICE_ID.fullmatch(device_id):
        raise ConfigError("invalid_mailbox_id")
    lockpath = path.with_name(path.name + ".voice-lock")
    descriptor = os.open(lockpath, os.O_RDWR | os.O_CREAT, 0o600)
    with os.fdopen(descriptor, "a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        read_credentials(path)
        data = private_json(path)
        existing = data["devices"].get(device_id)
        if existing:
            if existing.get("purpose") != PURPOSE or existing.get("revoked", False):
                raise ConfigError("mailbox_id_already_in_use_or_revoked")
            return False
        original = path.read_bytes()
        backup = path.with_name(path.name + ".before-voice-" + secrets.token_hex(4))
        with os.fdopen(os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as handle:
            handle.write(original)
            handle.flush()
            os.fsync(handle.fileno())
        data["devices"][device_id] = {"token": secrets.token_hex(32), "revoked": False, "purpose": PURPOSE}
        fd, temporary = tempfile.mkstemp(prefix=".voice-registry-", dir=path.parent)
        try:
            with os.fdopen(fd, "w") as handle:
                json.dump(data, handle, ensure_ascii=False, indent=2)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            if path.read_bytes() != original:
                raise ConfigError("credentials_changed_during_enrollment")
            os.replace(temporary, path)
        finally:
            Path(temporary).unlink(missing_ok=True)
    return True
