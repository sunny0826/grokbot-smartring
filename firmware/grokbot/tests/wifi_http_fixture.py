#!/usr/bin/env python3
"""Loopback browser fixture: production assets + C++ protocol, synthetic owner/radio.

Run: python3 firmware/grokbot/tests/wifi_http_fixture.py --port 8093
Reset: GET /__fixture?scenario=choosing|empty|failed|success|expired|offline|
       drop_accepted|drop_unaccepted|conflict|fail_next
Audit: GET /__fixture/events (command metadata only, never passwords)
Drop scenarios close the next POST connection without a response; they never replay it.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "f" * 32  # Synthetic fixture only.
LOCK = threading.Lock()
state = {}


def reset(scenario):
    state.clear()
    state.update(scenario=scenario, revision=2, scan_revision=1, start=time.monotonic(),
                 phase={"failed": "failed", "success": "connected"}.get(scenario, "choosing"),
                 transition=None, events=[])


def compile_protocol(work):
    cjson = ROOT / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c99", "-Wno-deprecated-declarations", "-I" + str(cjson),
                    "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")], check=True)
    executable = work / "protocol"
    subprocess.run(["c++", "-std=c++17", "-I" + str(ROOT / "main"), "-I" + str(cjson),
                    str(ROOT / "tests/wifi_http_fixture.cpp"), str(ROOT / "main/wifi_setup_http_protocol.cpp"),
                    str(work / "cjson.o"), "-o", str(executable)], check=True)
    return executable


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def send(self, status, data, kind="application/json"):
        if isinstance(data, str):
            data = data.encode()
        self.send_response(status)
        for name, value in {"Content-Type": kind, "Content-Length": str(len(data)),
                            "Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
                            "X-Content-Type-Options": "nosniff", "Connection": "close",
                            "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"}.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def drop(self):
        self.close_connection = True
        self.connection.shutdown(socket.SHUT_RDWR)
        self.connection.close()

    def do_GET(self):
        path = urlsplit(self.path)
        with LOCK:
            if path.path == "/__fixture":
                scenario = parse_qs(path.query).get("scenario", ["choosing"])[0]
                reset(scenario)
                self.send(200, json.dumps({"fixture": scenario})); return
            if path.path == "/__fixture/events":
                self.send(200, json.dumps(state["events"])); return
            static = {"/": ("html", "text/html; charset=utf-8"), "/app.js": ("js", "text/javascript; charset=utf-8"),
                      "/style.css": ("css", "text/css; charset=utf-8")}
            if path.path in static:
                extension, kind = static[path.path]
                self.send(200, (ROOT / "main" / ("wifi_setup_page." + extension)).read_bytes(), kind); return
            if state["scenario"] == "expired" or time.monotonic() - state["start"] >= 300:
                self.send(410, '{"error":"closed"}'); return
            if state["scenario"] == "offline":
                self.drop(); return
            if path.path == "/api/session":
                self.send(200, json.dumps({"operation": 7, "token": TOKEN})); return
            if path.path != "/api/status":
                self.send(404, '{}'); return
            if self.headers.get("X-Setup-Token") != TOKEN:
                self.send(403, '{}'); return
            transition = state["transition"]
            if transition and time.monotonic() >= transition[0]:
                state["phase"] = transition[1]; state["transition"] = None; state["revision"] += 1
            output = subprocess.check_output([self.server.protocol, state["phase"], str(state["revision"]),
                                               str(state["scan_revision"]), str(max(0, 300 - int(time.monotonic() - state["start"]))),
                                               "0", state["scenario"]])
            self.send(200, output)

    def do_POST(self):
        with LOCK:
            if self.path != "/api/command":
                self.send(404, '{}'); return
            if self.headers.get("X-Setup-Token") != TOKEN:
                self.send(403, '{}'); return
            length = int(self.headers.get("Content-Length", 0))
            if not 0 < length <= 512:
                self.send(413, '{}'); return
            raw = self.rfile.read(length)
            result = subprocess.run([self.server.protocol, "parse"], input=raw, capture_output=True)
            raw = b""
            if result.returncode:
                self.send(400, '{}'); return
            command = json.loads(result.stdout)
            action = ["scan", "connect", "cancel", "close"][command["action"]]
            event = {"action": action, "revision": command["revision"]}
            state["events"].append(event)
            if state["scenario"] == "expired" or command["operation"] != 7:
                event["result"] = 410; self.send(410, '{}'); return
            if command["revision"] != state["revision"] or state["scenario"] == "conflict":
                state["scenario"] = "choosing"; state["revision"] += 1
                event["result"] = 409; self.send(409, '{}'); return
            drop = state["scenario"] in ("drop_accepted", "drop_unaccepted")
            if state["scenario"] == "drop_unaccepted":
                state["scenario"] = "choosing"; event["result"] = "not_delivered"; self.drop(); return
            state["revision"] += 1
            if action == "close":
                state["scenario"] = "expired"
            elif action == "connect":
                state["phase"] = "connecting"
                state["transition"] = (time.monotonic() + 3, "failed" if state["scenario"] == "fail_next" else "connected")
            elif action == "scan":
                state["phase"] = "scanning"; state["scan_revision"] += 1
                state["transition"] = (time.monotonic() + 1.5, "choosing")
            else:
                state["phase"] = "choosing"; state["transition"] = None
            event["result"] = "accepted_lost_response" if drop else 202
            if drop:
                state["scenario"] = "choosing"; self.drop(); return
            self.send(202, '{"result":"accepted"}')


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8093)
    args = parser.parse_args()
    reset("choosing")
    with tempfile.TemporaryDirectory(prefix="wifi-browser-fixture-") as directory:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
        server.protocol = str(compile_protocol(Path(directory)))
        print(f"Production page fixture: http://127.0.0.1:{args.port}", flush=True)
        server.serve_forever()
