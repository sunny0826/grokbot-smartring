#!/usr/bin/env python3
"""Exercise real voice actor, journal, mailbox and LVGL with controlled external boundaries."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_voice_ui import make_png


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 12))
    parser.add_argument("--scenario", action="append", help="Run selected independent scenarios")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work = args.work_dir.resolve() if args.work_dir else Path(tempfile.mkdtemp(prefix="my-bot-voice-actor-"))
    work.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent / "voice_actor_smoke"
    build = work / "build"
    print(f"Build and evidence: {work}", flush=True)
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)
    scenarios = args.scenario or [
        "read-hold", "read-boot", "inbox-hold", "inbox-boot", "late-read", "stale-inbox",
        "scope-ready", "corrupt-start", "save-failure", "delete-failure", "multi-unread", "ui", "ui-save-failure",
        "restart-write", "restart-read", "restart-confirm", "restart-empty",
    ]
    state = work / "restart.nvs"
    if not args.scenario:
        state.unlink(missing_ok=True)
    for scenario in scenarios:
        command = [str(build / "actor"), scenario]
        if scenario.startswith("restart-"):
            command.append(str(state))
        subprocess.run(command, cwd=work, check=True, timeout=15)
    for ppm in sorted(work.glob("ui-*.ppm")):
        print(f"Preview: {make_png(ppm)}")
    print(f"PASS: {len(scenarios)} isolated processes; all host tasks joined")


if __name__ == "__main__":
    main()
