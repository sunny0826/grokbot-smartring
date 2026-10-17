# Final independent fix review

Read-only product review against baseline 759f03c and current integrated workspace, 2026-10-17. No product/test source edits, deployment, commit, external production requests, or delegation. Only this receipt and a temporary derived regression harness were written. Same host model isolated reviewer; not cross-model evidence.

## Residual findings

No confirmed residual product defect in the reviewed fixes after the coordinator’s final JSON-size correction. Implementation receipt `docs/implementation/voice-latency-next.md` was read and still has the explicitly pending progress/testing section; final results and hardware limitations remain for the coordinator to record.

During this review, an additional retry-boundary defect was reproduced: an oversized JSON response on an already reused connection filled the 1023-byte destination, then `is_complete_data_received == false` was interpreted as interruption before the size rejection. The temporary harness at `/tmp/my-bot-latency-work/final-review-http/test.cpp` demonstrated `OVERSIZED_JSON_RESULT ok=1 additional_requests=2 remaining_responses=0`. This was a format/size error being retried, contrary to the selected retry scope. Coordinator fixed it by rejecting declared oversize and a full destination before considering incomplete reception; official tests now cover declared length and unknown/chunked-style length on a reused connection. This finding is resolved, not outstanding.

## Independent evidence

- Final `python3 firmware/grokbot/tools/check_voice_http.py --idf-path /Users/guoxudong/esp/esp-idf-v5.5.3`: PASS, ASAN/UBSAN production HTTP/transport/protocol code with deterministic SDK mock. The final run includes the two oversized-JSON no-retry regressions. Initial pre-follow-up official suite also passed, illustrating the missing case.
- Existing `/tmp/my-bot-latency-work/integrated-actor/build/actor` independently passed `response-close-race`, `response-settings-race`, `timing-boot`, and `timing-storage`; each printed `all_host_tasks_joined=true`. Actor binary timestamp 20:22:27 is later than voice.cpp 20:19:05, scenarios.cpp 20:22:25, host.cpp 20:18:24, fixture.cpp 20:12:17 and voice_timing.cpp 20:13:27. No actor rebuild was needed; the later transport correction is exercised by the separate HTTP suite, not fixture transport.
- Final `git diff --check`: PASS.
- Source review confirms response applicability checks, update_reply/update_page, and published snapshot now occur within snapshot_lock, shared with input admission/settings ownership. An already accepted queued Close or Pending Settings therefore cannot be consumed by response publication. Identity, scope, generation and epoch checks remain strict. Rejected response pages are freed.
- BOOT send uses unknown physical release (0); touch release starts timing before journal persistence. Idempotent timing start preserves original release when storage subsequently succeeds. The two timing actor regressions exercise those outcomes.
- Response GET prepare explicitly requires `Connection: keep-alive`, pre-retires sessions at 28 seconds, and marks whether it actually reused a connection. Only the response GET path retries at most once after interrupted reuse; fresh-session failures, authorization and accepted-size malformed payloads do not create unbounded retries. Upload path is unchanged by this retry wrapper.
- ReplyReady is marked after validated response metadata or decoded replied JSON; PageValidated follows complete page/CRC validation. No confirmed regression found in the reviewed timing-stage move.
- Targeted Nowledge memory search completed for prior latency/release/reuse decisions; no speculative findings persisted.

## Limits

This reviewer did not rerun the coordinator’s full backend suite, full actor suite, LVGL suite, remote_core or IDF build. Final IDF incremental compilation is coordinator-owned. Host actor uses transport fixtures, HTTP checks use SDK mocks, and neither establishes physical ESP32 TLS reuse or last-DMA/panel timing. No hardware end-to-end latency or before/after speedup is claimed. The original transport review’s real loopback TLS evidence is prior-review evidence, not a new physical device test.

Final reviewed SHA-256:
- voice.cpp: 7c838a0f76836c239a26be520247e8ff4e9c7eba158f6b593725fb856d7586fe
- voice_http.cpp: fedf0e91214e02d7a61b3af5b96f92ae9e3bdec7e3f9ca07c2abea97b6e91bad
- voice_response_transport.cpp: 9bb519426279811ab2635ffd957740576be943090ea8d706a123d2c8d21d3b64
- voice_http_test.cpp: 10bfe37100d33fe6dde45bedaf08e714522df36efbebb8a79f62093833bacec4
