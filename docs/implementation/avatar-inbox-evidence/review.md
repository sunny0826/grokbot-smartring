# Avatar entry review

Reviewer: /root/avatar_entry_review, independent same-model review (not cross-model). Read-only product review; no firmware, service, serial, or product source modifications. Baseline: /tmp/my-bot-avatar-entry-baseline, 0.12.1. Review scope: this round's voice_ui.cpp, character_presenter.cpp/.h, gadget.cpp and voice_ui_smoke changes; direct touch, home-controls and Open dependencies only.

## Verdict
No blocking correctness issue found. The minor gesture-threshold inconsistency identified in the initial snapshot has been fixed and the added gesture assertions have been independently reviewed. Product source reviewed through the 17:49:48 character_presenter.cpp revision; final host logs were rechecked after the rebuild and both pass.

- Event ordering is correct: presenter attaches before main touch handlers and home controls (main.cpp:476-486). A new press is allowed through to initialize legacy swipe origin; tracked stationary pressing/release is stopped before legacy poke. Horizontal movement beyond 40 px is passed through and retains the original swipe start.
- Canvas images are not clickable by default in the vendored LVGL (image/lv_image.c:704), so screen handling is necessary. Both surfaces are registered. Original event target filtering excludes unrelated bubbled children; no added bubble flag creates duplicate delivery.
- Release checks Home, Idle, unread, unchanged generation and both overlays (character_presenter.cpp:75-79). Actor queue validates current generation again (voice.cpp:414-421), and handle validates it before Open (voice.cpp:221-224). open_reply checks unread and inbox identity (voice.cpp:189-200).
- Touch geometry follows the last drawn center and max ellipse radius, including canvas offset (character_presenter.cpp:132-137), rather than a fixed screen center.
- Delete callbacks clear each stored surface pointer; attach/destructor remove callbacks by this instance's user_data. Parent deletion delivers DELETE before child destruction in vendored lv_obj_tree.c:675 onwards, and the canvas DELETE then clears the framebuffer. No stale pointer dereference found.
- Every show_phase restores hint visibility before phase-specific changes (voice_ui.cpp:189); active progress phases then hide it. Error, recording and confirmation instructions remain available. Inbox button creation, update and delete bookkeeping were removed consistently.

## Low-impact observation

CLOSED P3: character_presenter.cpp:70 now uses std::abs(dx) > 40 || std::abs(dy) > 40, matching main.cpp. The initial radial-versus-axis discrepancy no longer exists in the source.

## Verification evidence and limits

Read /tmp/my-bot-avatar-entry-voice.log: real LVGL smoke passed, including pointer hold/release, slide cancellation, tap, paging, generation rejection, UI lifecycle and three-skin fusion. Read /tmp/my-bot-avatar-entry-wifi.log: Fused Wi-Fi cover PASS and Wi-Fi UI real LVGL PASS, followed by completed preview outputs. The added avatar_entry_checks uses real LVGL pointer input and a later event observer on both screen and canvas. It proves short avatar taps issue one Open and consume RELEASED; outside taps and horizontal drags reach later event handlers; long, generation-changed, unread-cleared and Wi-Fi-covered taps do not Open. It checks all three skins crossed with both default nonclickable and explicitly clickable canvas, moves the real draw_home center with the frame timer paused, verifies the minimum hit radius for a Thinking pose reduced to 8 px, and asserts seven progress stages expose only one title label or title plus return. These assertions were reviewed and the final test log contains Avatar entry PASS alongside fusion and voice UI PASS. The log explicitly recompiles character_presenter.cpp and smoke.cpp; the object timestamp 17:50:37 is later than the final source timestamp 17:49:48. The pending final host rebuild check is closed. The smoke does not link production main touch_cb: callback delivery plus static main control-flow inspection supports compatibility, but actual skin selection, physical touch, shaking and display feel were not tested by this reviewer.

## Comment review

No newly introduced product comments, suppression directives, disabled code or workaround commentary in the scoped diff. Deletion suggestions: 0; actual deletions by reviewer: 0; restored comments: 0; MUST KILL structural targets: none. The existing test comment about UI ownership of page bytes matches the adjacent mutation/assertion and remains outside this round's product-comment changes. No unrelated historical comments were audited or removed.

Targeted Nowledge search completed for my-bot 0.12.1 avatar/new-message animation context; implementation claims were checked against local source rather than accepted from memory.
