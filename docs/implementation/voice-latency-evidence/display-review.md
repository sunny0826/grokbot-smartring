# Display / actor review

Read-only review against baseline 759f03c and the current shared-workspace changes. No source edits, delegation, deployment, production request, or commit. Reviewer: isolated same-host-model subagent; not cross-model evidence. Targeted Nowledge memory search completed for `my-bot voice latency first visible timing actor`; prior records confirm the required boundary is physical release to visible complete page, not stored-to-replied.

## Findings

### P2 — An arriving response can consume the generation before an already accepted Close

Locations: `firmware/grokbot/main/voice.cpp:332–340`, `:403–412`; rejection in `voice_logic.h:96–102`.

Concrete legal interleaving:

1. Actor is Waiting/Stored, asynchronous response is active. At line 404 the actor finds no input, then enters the 30 ms delay at line 410.
2. UI accepts Close with the current published generation and queues it. The response finishes and is queued during this delay.
3. Actor resumes directly into mailbox_step. Its response guard checks the still-current generation, accepts it, and show_page increments the generation again.
4. Next iteration takes the queued Close; voice_input_current rejects its old generation. The accepted Close is lost and the reply remains open.

The response-close test avoids this interleaving by waiting until Idle before releasing the response. Existing generation checks correctly reject responses after Close has actually run, but do not preserve accepted control input that is waiting to run.

Fix: serialize response application with input reservation. Drain/handle accepted control input before applying a reply, or reserve response application under snapshot_lock only when no pending input, hold, or settings owner exists, so later input is ordered against a known published transition. Add a deterministic barrier test that queues Close after the loop's input read but before response application.

### P2 — Settings Pending does not invalidate a response until resume, allowing a pre-settings page to survive

Locations: `firmware/grokbot/main/voice.cpp:334–340`, `:379–383`, `:410–412`, `:465–492`; `voice_mailbox.cpp:24–25` calls finished before queuing result.

Concrete legal interleaving:

1. Actor passes boundary() while the response mailbox is active and then enters its 30 ms idle delay.
2. voice_settings_pause succeeds, sets Pending and invalidates HTTP. The configuration epoch is unchanged; it increments only in voice_settings_resume.
3. The already-successful response completes (or completes just before invalidation), clears mailbox_active and queues its page.
4. This same actor iteration reaches mailbox_step before the next boundary() call. The epoch and generation still match, so it installs the page while settings is Pending.
5. Next iteration enters Paused. Resume increments epoch, but there is no longer an old response to discard: current already is ViewingReply. Even a subsequent scope change only clears unread and bumps generation, leaving the old page in place.

This bypasses the intended discard-after-settings behavior and can preserve the old endpoint/scope's response across configuration changes. The existing response-settings scenario is scheduler-dependent because it has no barrier at this point.

Fix: reject/defer response application under the same lock when the settings gate owns the actor, and make epoch invalidation occur atomically with accepted pause or otherwise ensure no pre-pause result can be applied before the next boundary. Include a deterministic Pending-window regression. This overlaps the first finding's control-intent ordering root cause, but the consequence and test trigger differ.

### P2 — BOOT sends record press timestamps as physical release timestamps

Locations: `firmware/grokbot/main/voice_button.cpp:16–19`; `voice_logic.h:48–59,72–75`; `voice.cpp:238,459`.

VoiceButton::update returns true only on a debounced press, explicitly false on release. During Recording/Ready that Boot action becomes Send, and Input.at_us is passed to voice_timing_start as released_us. Holding the second BOOT press for several seconds therefore emits release_us and first_visible elapsed from a press before physical release; it can even declare first_visible while the button remains held. This is not the claimed physical-release metric.

Fix: distinguish send-trigger time and physical release, or pass unknown release for toggle BOOT sends until a real release event is associated with the request. Do not label the press edge release_us. Touch hold release uses its actual release handler and is correctly carried through the mailbox path. A related edge is storage failure: timing_start currently occurs after journal.set_pending; a failed first send loses the original release and a later Ready/Boot send starts timing at its retry press. Capturing the original request release before the storage attempt avoids that undercount too.

## Verified paths and evidence

- `python3 firmware/grokbot/tools/check_voice_timing.py --work-dir /tmp/my-bot-latency-work/display-review-tests` passed: timing core plus normal, covered, overlay, stale, fail-first, fail-middle, fail-last, boot real-LVGL processes. Each output and build artifact is under that directory.
- Scope-specific `git diff --check` passed.
- Timing frame token, loaded request ID/generation/index, opaque image and ancestors, screen/transition checks, and single-refresh bitset coverage reject old generations, partial redraw accumulation, visibility loss, and failed strips. Production GPIO TE hook is invoked after the wait observes DMA completion, with the timestamp captured before UI coverage accounting. Failure paths do not count flush_ready as success.
- The hook is synchronous on the serialized LVGL GPIO TE path, so the current token at callback time is the frame that owns that transfer; it is not an asynchronous callback carrying an unrelated newest token.
- A Close that has already run clears request_id and advances generation. New recording advances generation and changes request_id. Their returned old page buffers fail guards and are freed. During Recording/Preparing the old result remains queued and is later drained; no immediate leak from that deferral was found.
- Response first-page adoption does not call mark_read. Explicit mark_read still persists journal first, and remote confirmation still checks request ID and confirm_read. No read-journal bypass found.
- show_page gets request identity from the preceding accept_reply/phase publication; fallback page loading also publishes LoadingPage before direct publication. No missing current request ID found there.

## Boundaries

Actor execution was deliberately not attempted because the transport worker's Response API header/implementation was not yet integrated, as instructed. The concurrency findings above are source-proven interleavings, not claimed runtime reproductions. Recommend deterministic actor barriers after integration. Hardware DMA/panel behavior and complete IDF compilation remain coordinator checks. The successful UI suite uses host flush boundaries and is not physical display evidence. Other display modes deliberately do not emit first-visible timestamps. No timing/visibility P1 defect found in the reviewed GPIO TE completion path.
