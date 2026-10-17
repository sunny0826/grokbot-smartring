"""Run separately from the existing device gateway; never changes its configuration."""
import argparse
import ipaddress
import os
from pathlib import Path
import signal
import ssl
import sys

from .config import read_credentials
from .enroll import enroll
from .mailbox import MAILBOX_ID, Mailbox
from .server import Server
from .store import Store
from .worker import WhisperTranscriber, Worker


def main():
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "grok-gadgets"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8768)
    parser.add_argument("--credentials", type=Path, default=config / "credentials.json")
    parser.add_argument("--cert", type=Path, default=config / "lan/server.pem")
    parser.add_argument("--key", type=Path, default=config / "lan/server.key")
    parser.add_argument("--gateway-port", type=int, default=8764,
                        help="existing gateway's loopback device listener")
    parser.add_argument("--mailbox-id", default=MAILBOX_ID)
    parser.add_argument("--enroll-mailbox", action="store_true",
                        help="register the local software mailbox and exit; preserve existing devices")
    parser.add_argument("--data", type=Path, default=config / "voice")
    parser.add_argument("--model", default="base", help="faster-whisper model name or local directory")
    parser.add_argument("--preload-model", action="store_true",
                        help="download/load the local model and exit without starting HTTPS")
    parser.add_argument("--preload-assets", action="store_true",
                        help="download the pinned Chinese font and local speech model, then exit")
    parser.add_argument("--max-jobs", type=int, default=1000)
    parser.add_argument("--max-bytes", type=int, default=512 * 1024 * 1024)
    args = parser.parse_args()
    if (not 1 <= args.port <= 65535 or not 1 <= args.gateway_port <= 65535
            or args.max_jobs < 1 or args.max_bytes < 28844):
        parser.error("invalid port or storage limit")
    try:
        address = ipaddress.IPv4Address(args.host)
        allowed = address.is_loopback or any(address in ipaddress.IPv4Network(net)
            for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
        if not allowed:
            raise ValueError()
    except ValueError:
        parser.error("host must be a loopback or private LAN IPv4 address")
    os.umask(0o077)
    store = server = worker = mailbox = None
    def stop(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        if args.enroll_mailbox:
            changed = enroll(args.credentials, args.mailbox_id)
            print("Voice mailbox enrolled" if changed else "Voice mailbox already enrolled", flush=True)
            return 0
        if args.preload_model or args.preload_assets:
            if args.preload_assets:
                from .assets import preload_font
                preload_font()
            WhisperTranscriber(args.model).load(allow_download=True)
            print("Local voice assets loaded", flush=True)
            return 0
        read_credentials(args.credentials)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
        store = Store(args.data, args.max_jobs, args.max_bytes)
        server = Server((args.host, args.port), store, args.credentials, context)
        mailbox = Mailbox(store, args.credentials, device_id=args.mailbox_id, port=args.gateway_port)
        mailbox.start()
        worker = Worker(store, WhisperTranscriber(args.model))
        worker.start()
        print(f"Voice HTTPS receiver listening on port {args.port}; MCP mailbox connects via loopback {args.gateway_port}", flush=True)
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    except Exception:
        print("Voice receiver unavailable: check configuration, TLS, storage, and --enroll-mailbox setup.", file=sys.stderr)
        return 1
    finally:
        if server:
            server.server_close()
        if worker and worker.thread.is_alive():
            worker.stop()
        if mailbox:
            mailbox.stop()
        if store:
            store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
