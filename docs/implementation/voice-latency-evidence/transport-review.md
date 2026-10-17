# Transport independent review

Review snapshot: 2026-10-17, baseline 759f03c, main workspace integrated transport slice, before coordinator's follow-up fixes. Read-only product review; only this receipt written. No deployment, external request, source edits, or delegation.

## Findings (both accepted by coordinator; not yet re-reviewed after fixes)

### P2: Response requests never opt into server keep-alive

Location: `firmware/grokbot/main/voice_http.cpp:159-163` (prepare), initialize header setup at 104-105. `voice_bridge/src/voice_bridge/server.py:136-140` requires a single explicit `Connection: keep-alive` request header. Neither client path sets it; ESP-IDF 5.5.3 `esp_http_client.c` also does not set a default Connection header. Thus every actual new-server response says `Connection: close`, and the client tears down TLS. Host mock emits keep-alive regardless of request headers, so its reuse count assertions cannot detect the integration defect.

Suggested fix: set Connection: keep-alive only for the response session, check failure, assert request header in host tests, and make mock's keep-alive response contingent on opt-in or add real integration coverage.

### P2: Connection lifetime can interrupt an active response and stop polling

Locations: `firmware/grokbot/main/voice_http.cpp:145` checks age only at request start; `voice_response_transport.cpp:60-63,77` returns failure without recovery. `voice_bridge/src/voice_bridge/connections.py:61-64` starts a hard disconnect timer, including during active requests. Actor integration `voice.cpp:134-138` turns that failed poll into UploadFailed, ending automatic waiting.

A pending response started just before the client's 30-second cutoff may cross the server deadline while waiting up to one second. When first-response setup is sufficiently quick, client and server deadlines are close enough for this to occur. Generic stale pooled socket closure has the same path. This is a fault introduced/exposed by enabling pooling, rather than an upload retry requirement.

Actual current server + Store + loopback TLS reproduced active-wait interruption with a shortened lifetime: initial GET 200; subsequent /response after=stored (normal wait 1 s) failed RemoteDisconnected after 0.322 s with lifetime=0.25 s. No request went to production.

Suggested fix: retire pooled sessions with sufficient wait/transport margin and recover once from a transport failure on an already reused idempotent GET using a fresh connection. Do not retry uploads, auth/protocol errors, or unboundedly repeat. Coordinator selected early retirement by 2 seconds plus bounded reused-GET reconnect.

Reproduction command (run from repository root, no repository files modified):

```sh
PYTHONPATH=voice_bridge/tests:voice_bridge/src voice_bridge/.venv/bin/python - <<'PY'
from pathlib import Path
from tempfile import TemporaryDirectory
import http.client, time
import conftest
from test_latency import connection, fetch
with TemporaryDirectory(prefix='voice-review-') as d:
    root = Path(d)
    class Factory:
        def mktemp(self, name):
            p = root / name
            p.mkdir()
            return p
    tls = conftest.tls.__wrapped__(Factory())
    sg = conftest.storage.__wrapped__(root)
    store = next(sg)
    rg = conftest.receiver.__wrapped__(root, store, tls)
    receiver = next(rg)
    conn = connection(receiver)
    try:
        store.put(conftest.RID, conftest.DEVICE, conftest.wav())
        receiver[0].connections.lifetime_seconds = 0.25
        print('initial_response', fetch(conn)[0])
        started = time.monotonic()
        try:
            print('wait_response', fetch(conn, '/response', after='stored')[0])
        except (OSError, http.client.HTTPException) as e:
            print('active_wait_error', type(e).__name__, 'elapsed_seconds', round(time.monotonic()-started, 3))
    finally:
        conn.close()
        try: next(rg)
        except StopIteration: pass
        try: next(sg)
        except StopIteration: pass
PY
```

## Checks and other reviewed paths

PASS `python3 firmware/grokbot/tools/check_voice_http.py --idf-path /Users/guoxudong/esp/esp-idf-v5.5.3` with actual production transport/protocol sources, ASAN/UBSAN. PASS scoped git diff --check. Mock API is not actual TLS/SDK behavior; the findings above demonstrate that limitation.

Actual IDF source checked: open->connect->prepare resets parser per request while retaining connected transport; prepare_first_line removes stale GET Content-Length; missing delete_header returns ESP_ERR_NOT_FOUND; response completeness follows parser body count; cleanup closes/disposes client before credentials are wiped. CA backing storage remains alive through cleanup. Header callback strings are valid during callback. Message begin resets chunk state.

Cache ownership: mutex transfers the sole idle pointer; acquired active lease cannot be deleted by invalidate; changed epoch prevents it from being recached. Wi-Fi disconnect event increments epoch even for reconnect between polls. The complete initialized credential record is compared, including endpoint, token and CA; gadget_wifi_load initializes the record under configuration mutex. Scope validation and actor configuration/generation/ID checks cover normal settings handoff; settings gate waits for mailbox I/O completion. No additional confirmed ownership/settings defect found.

Response requests reset metadata and remove mutable request headers before use. Binary path requires one matching request ID/read field, type, exact declared page size and complete read; production voice_page_valid checks page index/count/shape/CRC. Errors close the handle; terminal responses do not keep TLS alive. Old server 404/405/501 records capability per configuration and continuously uses status + validated first-page fallback, with invalidation discarding capability. Legacy page failure returns failure, not a successful empty page. No additional confirmed validation/fallback defect found.

Targeted Nowledge search completed for prior latency/connection decisions; no speculative findings saved as durable memory.

Limits: no physical ESP32/TLS timing, no end-to-end latency measurements, no IDF build by this reviewer, no post-fix review yet. Existing mock suite passing must not be presented as proof of actual persistent transport reuse.
