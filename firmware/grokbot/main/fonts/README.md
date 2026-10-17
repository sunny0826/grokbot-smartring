# Voice UI font

`voice_font_22.c` and `voice_font_bitmap_*.inc` form a generated 22 px, 4 bpp subset of Noto Sans CJK SC Regular.
It contains printable ASCII and the Chinese/punctuation characters used by
`../voice_ui.cpp` and `../wifi_*.cpp`. It is intentionally not a general-purpose
CJK font. Wi-Fi network names use LVGL's separate 16 px common-CJK font, with
an escaped identifier for characters outside its coverage.

Upstream: [Noto CJK](https://github.com/notofonts/noto-cjk), release `Sans2.004`,
commit `523d033d6cb47f4a80c58a35753646f5c3608a78`.
The original font is `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`.
Its SHA-256 is `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`.
The upstream SIL Open Font License is preserved in `OFL.txt`; this subset is
distributed under that same license. The original OTF is downloaded only during
regeneration and is not checked into the firmware repository.

Run from the repository root after changing UI copy:

```sh
python3 firmware/grokbot/tools/generate_voice_font.py
```

This requires Python 3, Node.js, npm, and network access. The script pins
`lv_font_conv@1.5.3`, verifies the font hash, and uses stable relative input/output
names. For offline source reuse, pass `--source /path/to/NotoSansCJKsc-Regular.otf`;
npm must already have the converter cached. Bitmap compression and kerning are
disabled to minimize runtime dependencies. Generated bitmap data is split into chunks of at most 480 lines. The font includes LVGL 9
compatibility guards and needs no built-in LVGL font configuration.

Add `voice_ui.cpp` and `fonts/voice_font_22.c` to the main component's `SRCS`.
Call `voice_ui_create(screen)` after the existing screen widgets are created,
under the display lock. Use `voice_ui_active()` in the screen's touch handler
before pet gestures; the overlay also disables input event and gesture bubbling.
`CharacterPresenter::update(now)` refreshes the voice UI and character from one
snapshot in the main LVGL frame (33 ms target). There is no separate voice UI
timer. Attach the presenter to the shared canvas before starting that frame loop.

## Host UI check

Run the retained behavioral smoke against the project's LVGL source:

```sh
python3 firmware/grokbot/tools/check_voice_ui.py
```

Requires Python 3, CMake, a native C/C++ compiler, and the managed LVGL component
(run `idf.py reconfigure` in `firmware/grokbot` if it is missing). No ESP32 hardware,
network calls, font downloads, or Python image packages are needed by this check.
It builds in a temporary directory and prints paths to `recording.png`, `failure.png`, `reply-first.png`, and `reply-last.png` for visual inspection. Use `--work-dir /tmp/my-bot-voice-ui-check`
to reuse the build on subsequent runs. The output directory is intentionally
retained so the images remain available; it may be deleted after review.

Pass `--page /path/to/page.bin` to render a real VPG1 page produced by the Mac.
The smoke checks idle visibility, recording cancellation, stale generation
rejection, absent upload buttons, mailbox states, unread reopening, page controls,
explicit read, display buffer ownership, pet event isolation, and overlay recreation.
It also checks all three character skins, the first visible voice frame, static
error refreshes, and canvas hiding/restoration. `fusion-frames/` contains actual
LVGL captures of six injected voice phases for each skin; these are host previews,
not physical-device captures or end-to-end voice evidence.
It mocks only the voice snapshot/action boundary and uses actual LVGL rendering
and events; microphone, upload, and physical touch behavior still need device
verification.
