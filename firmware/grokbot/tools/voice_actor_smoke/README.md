# Real voice actor and LVGL host regression

Run from the repository root:

```sh
python3 firmware/grokbot/tools/check_voice_actor.py --work-dir /tmp/my-bot-voice-actor
```

The build stays in `--work-dir` and can be reused. Each scenario runs in a separate process, and every FreeRTOS replacement thread is joined. A scenario failure or a worker exception fails the command; a 15-second process timeout is a final protection against deadlocks.

The executable links production `voice.cpp`, `voice_mailbox.cpp`, `voice_journal.cpp`, `voice_ui.cpp`, `voice_motion.cpp`, `home_controls.cpp`, the real font and real LVGL. There is no replacement for `voice_action`, `voice_hold_begin`, `voice_hold_end`, `voice_snapshot`, or `voice_copy_page`. Only platform and transport boundaries are replaced.

| Scenario | Observable behavior |
| --- | --- |
| `read-hold`, `read-boot` | MarkRead returns Idle within 250 ms; a blocked Read cannot prevent the next hold or BOOT recording. |
| `inbox-hold`, `inbox-boot` | A blocked background Inbox cannot prevent recording. |
| `late-read` | Read(A) completes after recording and uploading B; B's phase and durable pending survive. |
| `stale-inbox` | Inbox(A) captured before a new recording cannot publish an unread badge after that recording is cancelled. |
| `scope-ready` | A Ready recording cannot upload under changed enrollment credentials; restoring the original enrollment permits retry. |
| `corrupt-start` | A corrupt journal leaves a visible storage failure; the actor explicitly deletes itself instead of returning from a FreeRTOS entry point. |
| `save-failure` | Commit failure preserves the reading page and pending request, sends no Read, and allows retry. |
| `delete-failure` | Commit failure while deleting an acknowledged receipt keeps it durable and retries after the backoff. |
| `multi-unread` | Failed Read(A) remains queued; Inbox excludes A and discovers B; both local receipts are retained. |
| `ui` | Real LVGL PRESSED/RELEASED/CLICKED events on the last-page 已读 button reach the real actor and hide the reading overlay while Read remains blocked. |
| `ui-save-failure` | A visible 保存失败 title and original page remain after commit failure; clicking 已读 again succeeds. |
| `restart-write/read/confirm/empty` | Four separate processes retain an unconfirmed receipt, recover it without active pending or unread resurrection, confirm it, then observe no retry after another restart. |

Select one scenario with `--scenario NAME`; repeat the option to select several. The restart scenarios share `restart.nvs` and must run in the listed order. The complete run removes that fixture before starting.

The transport fixture uses condition-variable barriers. A test cannot accidentally complete an HTTP Read or Inbox before the explicit permit. The retry test advances the external ESP clock by five seconds; production deadlines and code are unchanged. Journal inspection reloads the real journal through the NVS boundary, rather than reproducing its binary encoding in test assertions.

UI captures use a deterministic colored reply image. `ui-save-failure.png` shows the production title, image and buttons. `ui-read-home.png` proves the reading overlay is hidden; this target does not render the home character, so the capture has a plain screen background. Character animation is covered by the separate voice UI smoke.

The host NVS replacement supports string/blob queries and commits, committed state across processes, and failure before a commit changes durable state. It does not reproduce flash power loss, torn pages, uncertain NVS return codes, hardware endurance or partition exhaustion. This suite also does not validate real TLS, microphone quality, concurrent device memory use, physical touch or device latency. The 250 ms bound is host responsiveness, not an ESP32 performance measurement.

The pre-change synchronous actor was separately run with the same public-action/barrier approach in `/tmp/my-bot-async-read-probe/`: it remained MarkingRead and rejected the next recording while Read was blocked. That baseline artifact is not required to run this permanent suite.
