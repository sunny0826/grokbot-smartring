#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5,<4"]
# ///
"""Configure the ring over USB without putting passwords or device tokens in arguments."""
import argparse
import getpass
import json
import re
import time

import serial
from serial.tools import list_ports


def hostname(value):
    labels = value.split(".")
    if (len(value) > 127 or len(labels) < 2 or not any(c.isalpha() for c in value) or any(
            not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
            for label in labels)):
        raise argparse.ArgumentTypeError("Use a DNS hostname, without https://, port or path")
    return value.lower()


def choose_port(explicit):
    if explicit:
        return explicit
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if len(ports) != 1:
        raise RuntimeError("Connect one ESP32 by USB, or specify --port")
    return ports[0]


def response(connection, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        raw = connection.readline(4096)
        try:
            value = json.loads(raw)
        except (ValueError, UnicodeError):
            continue
        if isinstance(value, dict) and type(value.get("ok")) is bool:
            return value
    return None


def open_console(port):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        connection = None
        try:
            connection = serial.Serial(port, 115200, timeout=0.3, write_timeout=2)
            while time.monotonic() < deadline:
                connection.write(b'{"type":"status"}\n')
                status = response(connection, 1)
                if status and status["ok"]:
                    return connection, status
        except (serial.SerialException, OSError):
            pass
        if connection:
            connection.close()
        time.sleep(0.5)
    raise RuntimeError("No USB console response; close any serial monitor and retry")


def apply(port, command):
    connection, status = open_console(port)
    try:
        if "remote_host" not in status or "transport" not in status:
            raise RuntimeError("This firmware does not support remote configuration; flash 0.7.0 or later first")
        connection.write(json.dumps(command, ensure_ascii=True).encode() + b"\n")
        receipt = response(connection, 5)
        if not receipt or not receipt["ok"]:
            raise RuntimeError("Configuration was not acknowledged; previous settings may still be active")
    finally:
        connection.close()
    time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port; automatically selects a single ESP32")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--remote-host", type=hostname, help="Existing tunnel hostname")
    mode.add_argument("--lan", action="store_true", help="Return to the saved LAN endpoint")
    parser.add_argument("--network", action="store_true", help="Prompt locally for a Wi-Fi network and password")
    args = parser.parse_args()
    network = None
    if args.network:
        ssid = input("Wi-Fi / 手机热点名称: ")
        password = getpass.getpass("Wi-Fi / 热点密码（不会显示）: ")
        if not 1 <= len(ssid.encode()) <= 32 or not (not password or 8 <= len(password.encode()) <= 64):
            parser.error("SSID must be 1–32 bytes; password must be empty or 8–64 bytes")
        if password != getpass.getpass("再次输入密码: "):
            parser.error("Passwords do not match")
        network = {"type": "wifi_network", "ssid": ssid, "password": password}
    port = choose_port(args.port)
    if args.remote_host is not None or args.lan:
        apply(port, {"type": "remote", "host": args.remote_host or ""})
        print("连接方式已保存。")
    if network:
        apply(port, network)
        network.clear()
        print("Wi-Fi 已保存，设备正在重新连接。")
    connection, status = open_console(port)
    connection.close()
    visible = {key: status.get(key) for key in (
        "firmware", "transport", "remote_host", "link", "ssid", "clock_ready", "transport_error", "memory")}
    print(json.dumps(visible, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, serial.SerialException, OSError) as error:
        raise SystemExit(str(error)) from None
