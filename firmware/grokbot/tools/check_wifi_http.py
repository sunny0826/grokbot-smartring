#!/usr/bin/env python3
"""Compile production HTTP/protocol against deterministic IDF adapters and real cJSON."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
idf = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5.3"))
cjson = root / "managed_components/espressif__cjson/cJSON"
if not cjson.is_dir():
    cjson = idf / "components/json/cJSON"
with tempfile.TemporaryDirectory(prefix="wifi-http-") as directory:
    work = Path(directory)
    subprocess.run(["cc", "-std=c99", "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-I" + str(cjson),
                    "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")], check=True)
    # Match ESP-IDF EMBED_TXTFILES symbol layout, including terminal NUL.
    resources = work / "resources.S"
    lines = [".section .rodata" if os.uname().sysname != "Darwin" else ".section __TEXT,__const"]
    for extension in ("html", "js", "css"):
        symbol = "_binary_wifi_setup_page_" + extension
        lines.extend([f".global {symbol}_start", f".global {symbol}_end", f"{symbol}_start:",
                      f'.incbin "{root / "main" / ("wifi_setup_page." + extension)}"', ".byte 0", f"{symbol}_end:"])
    resources.write_text("\n".join(lines) + "\n")
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I" + str(root / "tests/wifi_http_shim"), "-I" + str(root / "main"), "-I" + str(cjson),
                    str(root / "tests/wifi_http_test.cpp"), str(root / "main/wifi_setup_http.cpp"),
                    str(root / "main/wifi_setup_http_protocol.cpp"), str(resources), str(work / "cjson.o"),
                    "-o", str(work / "wifi_http")], check=True)
    subprocess.run([str(work / "wifi_http")], check=True)
