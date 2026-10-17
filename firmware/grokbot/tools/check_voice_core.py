#!/usr/bin/env python3
"""Run firmware state, page validation and JSON protocol tests on the host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if not args.idf_path:
        parser.error("Set IDF_PATH or pass --idf-path for the firmware's cJSON source")
    root = Path(__file__).resolve().parents[1]
    cjson = args.idf_path / "components/json/cJSON"
    with tempfile.TemporaryDirectory(prefix="voice-core-") as directory:
        work = Path(directory)
        subprocess.run(["c++", "-std=c++17", "-I" + str(root / "main"),
                        str(root / "tests/voice_logic_test.cpp"), "-o", str(work / "logic")], check=True)
        subprocess.run([str(work / "logic")], check=True)
        subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", "-I" + str(root / "main"),
                        str(root / "tests/voice_settings_test.cpp"), "-o", str(work / "settings")], check=True)
        subprocess.run([str(work / "settings")], check=True)
        subprocess.run(["cc", "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")], check=True)
        subprocess.run(["c++", "-std=c++17", "-I" + str(root / "main"), "-I" + str(cjson),
                        str(root / "tests/voice_protocol_test.cpp"), str(root / "main/voice_protocol.cpp"),
                        str(work / "cjson.o"), "-o", str(work / "protocol")], check=True)
        subprocess.run([str(work / "protocol")], check=True)


if __name__ == "__main__":
    main()
