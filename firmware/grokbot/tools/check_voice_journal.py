#!/usr/bin/env python3
"""Compile the real voice journal against isolated fake NVS; never access a device."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    shim = root / "tests/voice_journal_shim"
    with tempfile.TemporaryDirectory(prefix="voice-journal-", dir="/tmp") as directory:
        binary = Path(directory) / "journal"
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(shim), "-I" + str(root / "main"),
            str(root / "main/voice_journal.cpp"), str(shim / "fake_nvs.cpp"),
            str(root / "tests/voice_journal_test.cpp"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
