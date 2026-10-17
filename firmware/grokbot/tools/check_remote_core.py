#!/usr/bin/env python3
"""Exercise the real WSS adapter against a bounded transport test double."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="remote-core-") as directory:
    for name, sources in {
        "transport": ["tests/remote_transport_test.cpp", "main/gadget_ws.cpp"],
        "http_clock": ["tests/remote_http_clock_test.cpp", "main/voice_http.cpp", "main/gadget_clock.cpp"],
        "config": ["tests/remote_config_test.cpp", "main/gadget_config.cpp"],
    }.items():
        executable = str(Path(directory) / name)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I" + str(root / "tests/remote_shim"),
                        "-I" + str(root / "main"), *[str(root / path) for path in sources],
                        "-o", executable], check=True)
        subprocess.run([executable], check=True)
