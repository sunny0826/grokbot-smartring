# GrokBot — Gravity-Reactive Bot on a Round ESP32-S3 Display

A Grok-bot-style character living on the **VIEWE SmartRing-Plus** (ESP32-S3-N16R8,
1.85" 360×360 round LCD, QMI8658A IMU). Tilt the device and the bot tumbles toward
the ground with jelly squash-and-rebound; lay it flat and it cycles through idle
animations (thinking dots / notification badge / orbiting rings). Tap the screen to
poke it.

The character and all animation constants are ported frame-by-frame from
[bloub](https://github.com/jeremy-prt/bloub) (MIT) — radial-contour body,
spherical-projection eyes, blink & gaze drift, measured easing curves.

## Features

- **Gravity physics** — tilt to roll, circular boundary with restitution bounce,
  velocity-aligned jelly squash, gaze follows the downhill direction
- **Idle carousel** — after 1.5 s lying flat: `idle → thinking → idle → notify → idle → orbit`,
  with 0.4–0.6 s pose morphs and blink-masked transitions; any tilt exits instantly
- **Poke** — tapping the screen kicks the bot against gravity and forces a blink
- **Tear-free pipeline** — TE-hardware-synced flush (ST77916 TE line on GPIO38),
  internal-RAM strip buffers, measured-wall-time physics step. ~28 fps measured,
  no tearing, no edge crawl

## Hardware

| Part | Detail |
|---|---|
| Board | VIEWE SmartRing-Plus (ESP32-S3-N16R8, 16 MB flash, 8 MB PSRAM) |
| Display | ST77916, 360×360 round, QSPI 40 MHz, TE line wired to GPIO38 |
| IMU | QMI8658A (BSP provides EMA-filtered tilt vector) |
| Touch | CST816S (tap = poke) |

## Build & Flash

Requires ESP-IDF **v5.5.3** (`~/esp/esp-idf-v5.5.3` on the dev machine).

```bash
# IDF was installed under Python 3.14; put its venv first, then export
export PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin:$PATH"
. ~/esp/esp-idf-v5.5.3/export.sh

cd firmware/grokbot
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor   # device enumerates as USB-Serial/JTAG
```

After editing `sdkconfig.defaults`, force regeneration:
`rm -f sdkconfig sdkconfig.old && rm -rf build && idf.py set-target esp32s3`.

## Repository Layout

```
firmware/grokbot/           ESP-IDF firmware project
├── main/
│   ├── main.cpp            Physics, carousel driver, LVGL canvas glue
│   ├── bot_render.*        RGB565 software rasterizer (ellipse/capsule/poly/arc, 2 px AA)
│   ├── bot_face.*          Spherical eye projection + liveliness (blink, gaze drift)
│   ├── bot_states.*        Idle/thinking/notify/orbit poses (ported from bloub)
│   └── bot_math.h          Easing/noise helpers (no newlib trig in hot paths)
├── components/
│   ├── smartring_plus/     Vendored BSP + TE enablement (TEON cmd, TE_SYNC registration)
│   └── esp_lvgl_adapter/   Vendored LVGL adapter + patch: vsync wait once per frame
├── sdkconfig.defaults      240 MHz, perf optimizations, 16 MB flash, 1000 Hz tick
└── dependencies.lock       Registry deps (managed_components/ is regenerated)
docs/
├── display-pipeline-notes.md   Post-mortem: 5 root causes from jank to 28 fps tear-free
└── board-guide.html            VIEWE board & toolchain onboarding notes (Chinese)
reference/                  Third-party sources, git-ignored (re-clone per .gitignore)
```

## Display Pipeline (the hard part)

Symptoms evolved through five independent layers; each fix exposed the next.
Full measurements and debugging recipes: [docs/display-pipeline-notes.md](docs/display-pipeline-notes.md).

| Layer | Root cause | Fix |
|---|---|---|
| Math | newlib `sqrtf`/`hypotf` are soft-float (~100 cycles) | squared-distance tests, Taylor edge approximation |
| Buffering | self-maintained dirty rect vs. alternating strip buffers → ghosting | invalidate whole canvas; LVGL syncs both buffers |
| Coverage | 1 px hard AA band + truncating RGB565 blend | 2 px smoothstep band + rounded blending |
| Sync | DMA writes race panel scan → tearing steps on moving edges | ST77916 TE output (cmd 0x35) + adapter `TE_SYNC` on GPIO38 |
| Throughput | adapter waited for vsync per 40-row strip (8×/frame → 12 fps) + PSRAM strip copies | patch: wait once per frame; strips moved to internal RAM (~58 → 35 ms/frame) |

Physics uses measured wall-time `dt` (clamped 4–50 ms) because TE gating makes the
real frame rate float — fixed-step dt would warp motion speed.

## Tuning

Physics constants in `main/main.cpp`: `ACC` (gravity gain), `DAMP`, `RESTIT`
(bounce), `PMAX` (travel limit), `SCALE` (bot radius px), `CANVAS` (320).
Carousel timing/thresholds: `CAROUSEL[]` in `main/bot_states.cpp`,
flat-detection thresholds in `main.cpp` (`gm < 1.1` enter, `gm > 2.0` exit).

## Credits & License

- Character design & animation constants: [bloub](https://github.com/jeremy-prt/bloub) by jeremy-prt, MIT
- Board BSP: [viewesmart/smartring_plus](https://components.espressif.com/components/viewesmart/smartring_plus), Apache-2.0 (vendored + patched)
- LVGL adapter: [espressif/esp_lvgl_adapter](https://github.com/espressif/esp-iot-solution), Apache-2.0 (vendored + patched)

Project code: MIT, see [LICENSE](LICENSE).
