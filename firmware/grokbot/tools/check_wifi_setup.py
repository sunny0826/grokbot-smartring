#!/usr/bin/env python3
"""Compile the production setup owner and STA adapter against deterministic IDF fakes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-setup-") as directory:
    executable = str(Path(directory) / "wifi_setup")
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", "-I" + str(root / "tests/wifi_shim"),
                    "-I" + str(root / "main"),
                    *[str(root / path) for path in ["tests/wifi_setup_test.cpp", "main/wifi_setup.cpp", "main/wifi_setup_driver.cpp"]],
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
