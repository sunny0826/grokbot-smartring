#!/usr/bin/env python3
"""Build the real LVGL Wi-Fi UI on the host and exercise its visible behavior."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def make_png(ppm: Path) -> Path:
    """Convert the smoke's RGB PPM capture without third-party image packages."""
    magic, dimensions, maximum, rgb = ppm.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(rgb) != width * height * 3:
        raise ValueError(f"Invalid screenshot: {ppm}")

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = ppm.with_suffix(".png")
    png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )
    return png


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, help="Reuse an out-of-tree build and keep screenshots here")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 12))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work = args.work_dir.resolve() if args.work_dir else Path(tempfile.mkdtemp(prefix="my-bot-wifi-ui-"))
    work.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent / "wifi_ui_smoke"
    build = work / "build"
    print(f"Build and screenshots: {work}", flush=True)
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Debug", "--parallel", str(args.jobs)], check=True)
    binary = build / "smoke"
    subprocess.run([str(binary)], cwd=work, check=True)
    for ppm in sorted(work.glob("*.ppm")):
        name = ppm.stem
        print(f"Preview: {make_png(work / (name + '.ppm'))}")


if __name__ == "__main__":
    main()
