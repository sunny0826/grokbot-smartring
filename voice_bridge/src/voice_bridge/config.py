"""Strict gateway-compatible device credentials."""
import hmac
import json
import os
import re
import stat
from pathlib import Path

DEVICE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,63}")


class ConfigError(ValueError):
    pass


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ConfigError("invalid_configuration")
        result[key] = value
    return result


def _constant(value):
    raise ConfigError("invalid_configuration")


def private_json(path):
    try:
        with Path(path).open("rb") as handle:
            mode = os.fstat(handle.fileno()).st_mode
            if not stat.S_ISREG(mode) or stat.S_IMODE(mode) != 0o600:
                raise ConfigError("configuration_requires_mode_0600")
            data = handle.read(1024 * 1024 + 1)
        if len(data) > 1024 * 1024:
            raise ConfigError("invalid_configuration")
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique,
                           parse_constant=_constant)
        if not isinstance(value, dict):
            raise ConfigError("invalid_configuration")
        return value
    except (OSError, UnicodeError, ValueError, RecursionError):
        raise ConfigError("configuration_unavailable") from None


def read_credentials(path):
    devices = private_json(path).get("devices")
    if not isinstance(devices, dict):
        raise ConfigError("invalid_credentials")
    for device_id, record in devices.items():
        if not DEVICE_ID.fullmatch(device_id) or not isinstance(record, dict):
            raise ConfigError("invalid_credentials")
        token = record.get("token")
        if (not isinstance(token, str) or not 16 <= len(token) <= 256
                or type(record.get("revoked", False)) is not bool):
            raise ConfigError("invalid_credentials")
    return devices


def authorized(path, device_id, token):
    record = read_credentials(path).get(device_id)
    expected = record["token"].encode() if record else b"\0" * 32
    return (hmac.compare_digest(expected, token.encode("utf-8", "surrogatepass"))
            and record is not None and not record.get("revoked", False))
