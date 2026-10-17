# 0.12.1 state animation adaptation — independent review

Reviewer: same-model independent agent, read-only product review. This is not a cross-model review.

## Scope and baseline

Compared the nine saved C++ headers/sources under `/tmp/my-bot-animation-adapt-baseline` with the current workspace. The final main.cpp is identical to the saved 0.12.0 baseline. Reviewed direct consumers in bot_character.cpp/.h, voice_ui.cpp, voice.cpp, voice_character_test.cpp, and the voice UI fixture/tests. Did not review unrelated accumulated work since HEAD 0b60829 or touch product files, device, serial ports, services, or deployment.

Targeted Nowledge search completed for `my-bot 0.12.0 动画 融合`; matching memory `88df7e73-fbeb-458e-8164-86bc08cfd1bc` confirmed the prior phase/display lifecycle and explicitly unverified physical experience.

## Findings

No blocking correctness finding in the final product diff inspected.

The initial Home-wide Notify override would suppress existing remote animation.trigger commands while unread and leave gadget_publish reporting a carousel animation that was not drawn. The coordinator independently identified and replaced that version during this review. The final code preserves main.cpp and instead overlays only notifX/notifY/notifR/notifA in CharacterPresenter::draw_home; this removes the identified regression.

The overlay copies the caller's BotPose before modifying it. CharacterRenderer::render and character_draw are synchronous and do not retain the pose pointer. Caller state, physics, gaze, carousel progression, dizzy behavior and morph snapshots therefore remain unchanged by the overlay. The badge follows the incoming body's center and bodyR; during Thinking it becomes correspondingly small. That is a visible design tradeoff rather than a lifetime defect.

Unread comes from the same VoiceSnapshot used to draw the UI. The clock starts on false→true and is consumed even under cover; generation changes and cover/uncover do not restart it. Cover still prevents drawing. Explicit read clears the real unread overlay; the pre-existing demonstration carousel can independently show Notify, which remains legacy behavior and is not removed in this patch.

Continuous Orbit changes only Orbit's return-to-rest and fade-out factors. The default remains OneShot for existing callers, and the render time still advances for rotating rings. LoadingPage/MarkingRead now share the same ongoing-process animation. Claimed calls the original Thinking pose, while Replied retains the existing original Notify path and immediate auto-open policy. Background/covered replies retain the prior non-stealing policy.

## Evidence and verification limits

Read the targeted pure tests for sustained 30-second Orbit motion, original Thinking parameters/pixel changes, and default OneShot decay. Read real-LVGL UI tests for three-skin unread appearance, settled frame stability across generation updates, cover stability, uncover stability, and unread clearing. The provided render log reports PASS. After the final badge-overlay revision, independently re-read `/tmp/my-bot-state-animation-voice.log` and `/tmp/my-bot-state-animation-wifi.log`: both final reruns report PASS, including fused UI, pointer/gesture/inbox/read behavior and Wi-Fi cover/restore.

The suggested narrow regression was added and reviewed as `notification_preserves_pose`: all three skins × Thinking/Dizzy/Orbit pass actual Presenter.draw_home parameters, compare before/after pixels, constrain all changed pixels to the badge neighborhood, require nonzero changes and assert the input BotPose is unchanged. This addresses the non-Idle overlay risk; final voice smoke reaches its PASS after executing those assertions. Full main-loop IMU/remote-command interaction and real-device animation experience were not executed by this reviewer. The coordinator reports a benchmark-OFF firmware build of 0x1c46f0 bytes; this reviewer did not inspect that build log or flash the device. No performance or physical-experience claim follows from this review.

## Comment audit

The final product diff removes exactly two comment lines in bot_states.cpp: the unconditional fade-out narration and its per-ring-stagger continuation. The first would be inaccurate for Continuous; the second duplicates the directly inspectable shared renderer and is not a public API/platform/legal requirement. Removal accepted. No newly added comments, correctness suppressions, disabled code, or legal/license deletions appear in the reviewed diff. Existing unchanged source attribution, public interface comments and nearby port-design explanation were not expanded into unrelated cleanup. No MUST KILL structural target, restored comments, or unexecuted external constraint was found in the final scoped patch.
