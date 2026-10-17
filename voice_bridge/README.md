# Smartring voice mailbox

Touch recordings reach this Mac service over LAN HTTPS **8768** or the optional
public HTTPS **443** tunnel, are saved
in SQLite, and are transcribed locally. The service registers a **real software
mailbox** with the existing gadget gateway's loopback device listener, normally
**8764** in this deployment. Grok uses the existing OAuth MCP connector and its
six tools. An optional Grok Bot routine webhook wakes that same Bot after
transcription; the existing MCP still handles claims and replies.
There is no separate model assistant or gateway fork.

## Start

From the repository root:

```sh
uv sync --project voice_bridge --extra transcription
uv run --project voice_bridge --extra transcription voice-bridge --preload-assets
uv run --project voice_bridge --extra transcription voice-bridge --enroll-mailbox
uv run --project voice_bridge --extra transcription voice-bridge --host YOUR_PRIVATE_LAN_IP
```

`--preload-assets` explicitly downloads the pinned Noto Sans CJK SC font and
faster-whisper `base` model. Normal operation only reads cached assets. If a
SOCKS proxy prevents downloading, prefix the preload command with
`env -u ALL_PROXY -u all_proxy -u HTTP_PROXY -u http_proxy -u HTTPS_PROXY -u https_proxy`.
Transcription runs on CPU with int8 inference. `--model` accepts a cached model
name or directory; `--preload-model` only prepares the speech model and exits. Normal service startup
loads the cached model and font before accepting HTTPS traffic, keeping the same
instances in the running process.

Enrollment atomically adds `smartring-voice-mailbox` with its own random token to
`~/.config/grok-gadgets/credentials.json`, preserving existing devices and saving
a private backup. Repeating it is safe; an unrelated or revoked existing ID is
rejected. The running gateway re-reads this registry, so it needs no restart.
The mailbox does not impersonate the physical screen or reuse its session.

The default HTTPS bind is loopback; a physical device requires an explicit
private IPv4 `--host`. `--gateway-port` selects the existing loopback device port.
TLS uses the existing `lan/server.pem` and `lan/server.key`; the firmware trusts
the provisioned private CA. For remote access, the tunnel verifies this same
private CA at the origin; the screen verifies the public hostname and CA bundle.
See [remote setup](../docs/remote-access.md) for the exact authenticated routes.

The separate `voice-device-relay` process binds **127.0.0.1:8770**, accepts WSS
control via the tunnel, and forwards each authenticated session to the existing
**127.0.0.1:8764** device listener. It does not own the voice database or worker:

```sh
uv run --project voice_bridge voice-device-relay
```

The relay rejects invalid upgrades before opening the gateway connection. Its
default limits are 16 connections, 20 application frames/second, 16 KiB inbound,
2 KiB outbound, a two-frame receive queue, and bounded handshake/idle timeouts.
All device HTTP and upgrade responses use `Cache-Control: no-store`.

Mac must remain awake. This service has no automatic startup installation.
Stop its foreground process with Ctrl+C; restart with the same data directory.

## Use the existing MCP connector

1. `gadgets_list_devices`: find model **SmartRing voice mailbox (Mac)** with
   capabilities `voice.claim`, `voice.read`, `voice.reply`.
2. `gadgets_get_state`: inspect `requests`, `pending_count`, `unread_count`.
3. `gadgets_command`: claim a `ready` request with `voice.claim`, its
   `request_id`, and a fresh 32-character lowercase hexadecimal `claim_id`.
4. Read **this command's** `command.reported_state.result`. If the receipt is
   still accepted/dispatched, query `gadgets_command_status`; do not read a
   later device state as the command's result.
5. Process the transcript only after a successful claim. `next_offset` is null
   when complete; otherwise use `voice.read` with that offset for the next
   800-character page. Retain the claim ID when recovering from interruption.
6. Use `voice.reply` with the same request/claim IDs and at most **128 Unicode
   characters** of plain text. Success means the immutable reply and pages were
   saved. It does not mean the user saw or read them.

A duplicate claim reports `duplicate: true` and the current task status. A
completed request returns its saved reply; do not execute it again. After an
uncertain external action, inspect the Bot's own task history before continuing.
A different claim cannot take over an already claimed request. There is no
automatic expiration or reassignment. `voice.read` recovers durable status and
reply text without revealing the claim ID.

Normal short conversation needs two command calls, claim and reply. The gateway's
default limit of ten commands per minute is shared with physical device controls.
The largest allowed reply command, including 128 non-BMP characters, bounded IDs
and JSON escaping, fits the gateway's 2048-byte response frame. Device-to-gateway
hello/state/ack frames are independently bounded to 16 KiB.

**Automatic activation:** configure a webhook routine on the existing Bot and
start with `--grok-trigger /private/path/trigger.json`. See
[automatic setup and recovery](../docs/automatic-voice.md). The sender uses the
Bot routine Bearer/HTTP 200 contract, not the separate HMAC/202 automation API.
The notification carries IDs only; a new Bot run must obtain a fresh claim
before executing. An accepted notification is not a completed reply.
Without this option, say “处理圆屏语音” in the existing Bot as before.

## State and recovery

`stored → transcribing → ready → claimed → replied`; a local transcription
failure is `failed`. Read status is a separate field set only by the screen's
explicit final-page action. Returning to the pet keeps the reply unread. The
screen discovers unread replies through the server inbox after reconnect/reboot.
There is no claim that an HTTP page download proves physical display.

WAV and job are saved in one SQLite transaction with synchronous FULL and macOS
fullfsync before upload acknowledgement. Identical upload IDs/content replay
safely; changed audio or device ownership conflicts. Claims are transactional
and their IDs are unique across requests. Identical replies replay safely;
changed text conflicts. Reply text and all rendered pages commit together.
A process lock permits one database owner; all local actors share its locked Store.

Interrupted transcription resumes. Claims and replies survive restarts.
Historical unsent `needs_configuration` jobs with transcripts migrate to ready.
Legacy submitted/dispatching/unconfirmed jobs become `needs_review` and are never
automatically rerun. The old HMAC webhook sender/configuration remains removed;
the optional Bot routine trigger has a separate private configuration.

## Device endpoints

All endpoints authenticate the existing device token and `X-Device-ID` before
reading an upload body. Registry revocation is checked on every request.

| Method and path | Result |
| --- | --- |
| PUT `/v1/recordings/{id}` | 201 after durable save, 200 for identical replay |
| GET `/v1/recordings/{id}` | status, page_count, read, optional error |
| POST `/v1/recordings/{id}/retry` | retry a definite local transcription failure |
| GET `/v1/inbox` | oldest unread reply, or empty |
| GET `/v1/recordings/{id}/pages/{index}` | immutable binary page belonging to this device |
| POST `/v1/recordings/{id}/read` | idempotent explicit read acknowledgement |

Audio is a canonical 44-byte WAV header, 48 kHz, mono signed PCM16, 0.3–30
seconds, maximum 2,880,044 bytes. Chunked uploads and Expect:100-continue are not
supported. At most 16 handlers run with 15-second socket timeouts.

Reply pages use pinned Noto Sans CJK SC 22px, with its SIL OFL license under
`fonts/`. The font cache is `~/.cache/my-bot/NotoSansCJKsc-Regular.otf` (or
XDG_CACHE_HOME). Missing glyphs become □ and produce an explicit reply warning.
No arbitrary Unicode coverage is claimed. Each page has a 16-byte little-endian
`<4sHHHHI` header: VPG1, width=272, height=168, zero-based page index, page count,
CRC32 of the payload. The payload is 91,392 bytes of RGB565 little-endian pixels.
At most eight pages are accepted; text is not silently truncated.

## Storage and verification

Private data lives under `~/.config/grok-gadgets/voice/jobs.sqlite3` in a 0700
directory. The default limit is 1000 jobs and 512 MiB of audio plus page payloads;
metadata/transcripts add overhead. No recording or reply is automatically deleted.
Stop the service and preserve the database before deliberate retention maintenance;
deleting jobs also deletes replay protection. The existing gateway's private
request audit log can contain the transcript and reply returned by MCP tools.

```sh
uv run --project voice_bridge --extra transcription pytest -q voice_bridge/tests
uv run --project voice_bridge --extra transcription python \
  voice_bridge/tools/verify_speech.py /path/to/48k-mono-pcm16.wav --expect 你好
```

Tests use real temporary HTTPS servers and the pinned gateway/MCP SDK on isolated
ports, including claim/reply, gateway and store restart, maximum wire frames,
authentication, revocation, conflicts, storage limits and read ownership. Speech
verification uses cached real Whisper with an isolated receiver. These tests do
not stand in for the actual Bot conversation or physical microphone/display.

PyAV remains pinned below 19 for faster-whisper 1.2.1 decoder compatibility
([upstream issue](https://github.com/SYSTRAN/faster-whisper/issues/1589)).

## Stage timings

Version 0.3.1 records bounded, content-free events alongside job state in SQLite.
To inspect recent jobs without sending MCP commands:

```sh
python3 voice_bridge/tools/voice_timings.py --last 5
```

The report separates upload-body read, storage, transcription, notification,
claim and reply rendering. Durations between events use a monotonic clock only
within the same service process. A restart begins a new clock domain. Historical
jobs have no reconstructed timing events. Upload-body timing excludes the TLS
handshake, and fetching a reply page does not establish physical display time.
