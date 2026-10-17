#!/usr/bin/env python3
"""Compare incremental framebuffer and panel pixels with a full redraw oracle."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--work-dir", type=Path)
args = parser.parse_args()
firmware = Path(__file__).resolve().parent.parent
work = args.work_dir or Path(tempfile.mkdtemp(prefix="my-bot-render-"))
work.mkdir(parents=True, exist_ok=True)
main = firmware / "main"
sources = [main / f"{name}.cpp" for name in (
    "bot_render", "bot_face", "bot_states", "bot_character", "bot_character_pika", "bot_character_rowlet")]
binary = work / "pixels"
subprocess.run(["c++", "-std=c++17", "-O2", "-g", "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer", "-I", str(main), "-I", str(firmware / "sim/shim"),
    str(firmware / "tools/render_smoke/pixels.cpp"), *map(str,sources), "-o", str(binary)],check=True)
subprocess.run([str(binary)],check=True)

motion = work / "motion"
subprocess.run(["c++", "-std=c++17", "-O2", "-g", "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer", "-I", str(main), str(firmware / "tests/bot_motion_test.cpp"),
    str(main / "bot_motion.cpp"), "-o", str(motion)],check=True)
subprocess.run([str(motion)],check=True)

voice = work / "voice_character"
subprocess.run(["c++", "-std=c++17", "-O2", "-g", "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer", "-I", str(main), "-I", str(firmware / "sim/shim"),
    str(firmware / "tests/voice_character_test.cpp"), str(main / "voice_character.cpp"),
    *map(str, sources), "-o", str(voice)],check=True)
subprocess.run([str(voice)],check=True)

subprocess.run(["cmake", "-S", str(firmware / "tools/render_smoke"), "-B", str(work / "build"),
    "-DCMAKE_BUILD_TYPE=Release"],check=True)
subprocess.run(["cmake", "--build", str(work / "build"), "--parallel", "10"],check=True)
subprocess.run([str(work / "build/render_ui")],check=True)
