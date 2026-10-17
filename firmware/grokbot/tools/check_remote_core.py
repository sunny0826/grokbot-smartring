#!/usr/bin/env python3
"""Exercise the real WSS adapter against a bounded transport test double."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="remote-core-") as directory:
    for name, sources in {
        "transport": ["tests/remote_transport_test.cpp", "main/gadget_ws.cpp"],
        "http_clock": ["tests/remote_http_clock_test.cpp", "main/voice_http.cpp", "main/gadget_clock.cpp",
                       "main/voice_scope.cpp", "main/voice_transport.cpp", "main/voice_page_transport.cpp"],
        "scope": ["tests/voice_scope_test.cpp", "main/voice_scope.cpp"],
        "config": ["tests/remote_config_test.cpp", "main/gadget_config.cpp"],
    }.items():
        executable = str(Path(directory) / name)
        scope_test = name in ("http_clock", "scope")
        includes = ["-I" + str(root / "tests/scope_shim")] if scope_test else []
        libraries = ["-lcrypto"] if scope_test and sys.platform != "darwin" else []
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", *includes, "-I" + str(root / "tests/remote_shim"),
                        "-I" + str(root / "main"), *[str(root / path) for path in sources],
                        "-o", executable, *libraries], check=True)
        subprocess.run([executable], check=True)
