#!/usr/bin/env python3
"""Check bounded voice timings and real LVGL fade/coverage. No real DMA is simulated as hardware evidence."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, **kwargs):
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def main():
    firmware = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--lvgl", type=Path, default=firmware / "managed_components/lvgl__lvgl")
    parser.add_argument("--ui-source", type=Path, help="Alternate UI source for regression baseline")
    args = parser.parse_args()
    work = args.work_dir or Path(tempfile.mkdtemp(prefix="voice-timing-"))
    work.mkdir(parents=True, exist_ok=True)
    main_source = firmware / "main"
    run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-I", main_source, main_source / "voice_timing.cpp", firmware / "tests/voice_timing/core.cpp",
         "-o", work / "core"])
    run([work / "core"])
    config = ["cmake", "-S", firmware / "tests/voice_timing", "-B", work / "build",
              "-DCMAKE_BUILD_TYPE=Debug", f"-DLVGL={args.lvgl.resolve()}",
              f"-DTIMING_UI_SOURCE={args.ui_source.resolve() if args.ui_source else ''}"]
    run(config)
    run(["cmake", "--build", work / "build", "--parallel", str(min(os.cpu_count() or 1, 12))],
        stdout=subprocess.DEVNULL)
    for case in ("normal", "covered", "overlay", "stale", "fail-first", "fail-middle", "fail-last", "boot"):
        run([work / "build/timing_ui", case])
    print(f"PASS: timing core and 8 real LVGL scenarios; artifacts {work}")


if __name__ == "__main__":
    main()
