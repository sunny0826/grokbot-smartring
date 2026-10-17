#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["PyYAML>=6,<7"]
# ///
"""Add device paths to an existing local Cloudflare tunnel; preserve its MCP/OAuth routes."""
import argparse
from copy import deepcopy
from datetime import datetime, timezone
import ipaddress
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.parse import urlsplit

import yaml

CONTROL_PATH = r"^/v1/control$"
VOICE_PATH = r"^/v1/(inbox|recordings/[0-9a-fA-F]{32}(/(retry|read)|/pages/[0-9]{1,3})?)$"


def device_routes(config, hostname, voice_origin, ca_pool, relay_port=8770):
    result = deepcopy(config)
    ingress = result.get("ingress")
    if not isinstance(ingress, list):
        raise ValueError("The existing configuration needs ingress rules")
    origin = urlsplit(voice_origin)
    address = ipaddress.ip_address(origin.hostname or "")
    if (origin.scheme != "https" or origin.username or origin.password or origin.path
            or origin.query or origin.fragment or not address.is_private or not origin.port):
        raise ValueError("Voice origin must be an HTTPS private IP and explicit port")
    additions = [
        {"hostname": hostname, "path": CONTROL_PATH, "service": f"http://127.0.0.1:{relay_port}"},
        {"hostname": hostname, "path": VOICE_PATH, "service": voice_origin,
         "originRequest": {"caPool": str(ca_pool), "originServerName": origin.hostname}},
    ]
    original = next((rule for rule in ingress if rule.get("hostname") == hostname and not rule.get("path")), None)
    if original is None or original.get("service") != "http://127.0.0.1:8766":
        raise ValueError("Expected the original hostname route to http://127.0.0.1:8766")
    remaining = []
    for rule in ingress:
        if rule.get("hostname") == hostname and rule.get("path") in (CONTROL_PATH, VOICE_PATH):
            expected = next(item for item in additions if item["path"] == rule["path"])
            if rule != expected:
                raise ValueError("An existing device route differs; review it before replacing it")
        else:
            remaining.append(rule)
    insertion = remaining.index(original)
    result["ingress"] = remaining[:insertion] + additions + remaining[insertion:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path.home() / ".cloudflared/config.yml")
    parser.add_argument("--hostname", required=True)
    parser.add_argument("--voice-origin", required=True, help="For example https://192.168.1.10:8768")
    parser.add_argument("--ca-pool", type=Path, default=Path.home() / ".config/grok-gadgets/lan/ca.pem")
    parser.add_argument("--relay-port", type=int, default=8770)
    parser.add_argument("--apply", action="store_true", help="Back up and replace the validated config; does not restart cloudflared")
    args = parser.parse_args()
    if not 1 <= args.relay_port <= 65535 or not args.ca_pool.is_file():
        parser.error("A valid relay port and existing CA file are required")
    config = yaml.safe_load(args.config.read_text())
    updated = device_routes(config, args.hostname, args.voice_origin, args.ca_pool.resolve(), args.relay_port)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yml", prefix="grok-tunnel-", delete=False,
                                     dir=args.config.parent) as handle:
        candidate = Path(handle.name)
        os.chmod(candidate, 0o600)
        yaml.safe_dump(updated, handle, sort_keys=False)
    try:
        subprocess.run(["cloudflared", "--config", str(candidate), "tunnel", "ingress", "validate"], check=True)
        if not args.apply:
            print("Device routes validated. Add --apply to save; the running tunnel has not changed.")
            return
        if config == updated:
            print("Device routes already match; no change.")
            return
        suffix = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        backup = args.config.with_name(args.config.name + ".before-remote-" + suffix)
        shutil.copy2(args.config, backup)
        backup.chmod(0o600)
        os.replace(candidate, args.config)
        print(f"Saved. Backup: {backup}\nRestart the existing cloudflared process to activate these routes.")
    finally:
        candidate.unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
