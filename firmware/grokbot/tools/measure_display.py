#!/usr/bin/env python3
"""Capture the optional on-device A/B firmware's bounded, content-free display statistics."""
import argparse
import json
from pathlib import Path
import re
import time

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--seconds", type=int, default=210)
args = parser.parse_args()
if not 1 <= args.seconds <= 600:
    parser.error("--seconds must be 1..600")
connection = serial.Serial(port=None, baudrate=115200, timeout=.2, write_timeout=1)
connection.dtr = False
connection.rts = False
connection.port = args.port
connection.open()
start = time.monotonic()
next_status = 0
records = []
finished = False
try:
    while time.monotonic() - start < args.seconds:
        elapsed = time.monotonic() - start
        if elapsed >= next_status:
            connection.write(b'{"type":"status"}\n')
            next_status = elapsed + 5
        line = re.sub(r"\x1b\[[0-9;]*m", "", connection.readline(4096).decode(errors="replace")).strip()
        marker = "display_benchmark: "
        if marker in line:
            payload = line.split(marker, 1)[1]
            if payload == "complete":
                finished = True
                break
            row = json.loads(payload)
            records.append({"kind": "benchmark", **row})
            print(json.dumps(row), flush=True)
        elif "Draw bitmap failed" in line or "TE wait timed out" in line:
            records.append({"kind": "display_error", "elapsed": elapsed,
                            "error": "draw_bitmap" if "Draw bitmap" in line else "te_timeout"})
        elif "Guru Meditation" in line or "Stack canary" in line or "Brownout" in line:
            records.append({"kind": "device_fault", "elapsed": elapsed})
        else:
            try:
                status = json.loads(line)
            except ValueError:
                continue
            if status.get("ok") and "firmware" in status:
                records.append({"kind": "status", "elapsed": elapsed, **{
                    key: status[key] for key in ("firmware", "memory", "voice", "display", "link") if key in status}})
finally:
    connection.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"complete": finished, "records": records}, indent=2))
if not finished:
    raise SystemExit("Benchmark completion not observed; partial capture preserved")
