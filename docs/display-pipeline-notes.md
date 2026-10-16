# Display Pipeline Post-Mortem (SmartRing-Plus / ESP32-S3 / ST77916)

> 2026-10-16 — debugging log for the gravity animation. Symptom evolution:
> jank → sluggish feel → ghosting/garbage → static edge aliasing → moving staircase steps.
> Every symptom was a separate layer; fixing the surface exposed the next one down.
> Measure first, attribute second, patch last.

## Wi-Fi firmware regression: DMA allocation failure (2026-10-16)

Live serial logs showed `internal heap: free=328791 largest=233472`, followed by
`alloc primary buffer 259200 bytes failed`. The BSP retried in PSRAM, but each
screen flush then failed with `Failed to allocate priv TX buffer` and
`Draw bitmap failed: ESP_ERR_NO_MEM`.

The configured `buffer_height=40` was ignored: the adapter selected FULL mode for
TE_SYNC, allocating 360×360×2 bytes. PSRAM fallback still required a same-sized
internal DMA staging allocation in this SPI path. Initializing the display did
not prove that it could transmit pixels.

The local adapter now honors an explicit partial buffer height with TE_SYNC.
The BSP uses one 28,800-byte internal DMA buffer and the same 40-row SPI transfer
limit, without a PSRAM fallback. The v9 bridge waits for TE once per frame, keeps
that state per display, waits for DMA completion before reusing each strip, and
measures transmission from the first strip through the last. Startup logs print
the actual render mode, buffer size/count and memory type.

After building and flashing the app (ELF SHA256 prefix `7429f70e1`), a 45-second
serial observation confirmed `mode=partial, bytes=28800, count=1, memory=internal`,
zero draw/allocation errors and zero TE wait timeouts. Two 300-frame raster
reports were observed; their 18.978-second interval corresponds to about 15.8 fps
in that run. The single boot occurred when opening the serial connection. No
TLS connection was observed, so this run does not validate connected Wi-Fi/TLS
load or visually prove the absence of tearing.

The measurements below are historical. The old descriptions of two strip buffers
and guaranteed tear-free timing did not match the FULL-mode path above. Strip
copy time, inter-strip gaps and Wi-Fi scheduling must be included when checking
the panel timing; bus bandwidth alone cannot prove the absence of tearing.

## Hardware / pipeline facts (expensive to re-measure)

- Panel: ST77916, 360×360, QSPI 40 MHz, RGB565, TE line wired to GPIO38 (unused by stock BSP)
- LVGL adapter (esp_lvgl_adapter): PARTIAL render mode with one 40-row internal DMA buffer
  (a full-screen single flush needs a 259 KB internal-RAM bounce buffer → `ESP_ERR_NO_MEM`)
- Panel TE output: historically measured at ~62 Hz; unsynchronized writes risk tearing
- PSRAM: sequential u16 write ~120 ns/px, read-modify-write ~235 ns/px; internal RAM is ~10× faster
- **newlib `sqrtf`/`hypotf` are soft-float (~100 cycles each)** — banned from hot paths;
  use squared distances and the Taylor edge approximation `(d²-1)/2 ≈ d-1`

## Layered root causes & fixes

### 1. Jank (49 ms/frame) — math library
`hypotf` in the rasterizer hot loop. Replace all coverage/distance tests with squared
distance; enable compiler PERF optimization.

### 2. Ghosting / garbage — buffer consistency
With a self-maintained dirty rect + in-place canvas overwrite, the invalidated region must
cover `new ∪ old` consistently across every underlying buffer. Simple and reliable:
**invalidate the whole canvas** (`lv_obj_invalidate`) so the entire updated canvas reaches the panel.
With rasterization at ~10 ms, a full-canvas refresh fits the frame budget.

### 3. Static edge aliasing — coverage precision
- A 1 px linear transition band jumps between the few RGB565 gray levels during sub-pixel motion
- Fix: widen the band to 2 px + smoothstep shaping (`c*c*(3-2c)`) + rounded blending
  (truncation systematically darkens)
- Bounding-box padding must grow with the band (1.5 → 2.5 px) or the soft edge gets clipped

### 4. Motion tearing — frame sync
DMA writing GRAM while the panel scans → the edge shows a displacement step at the tear line.
Fix: append `TEON (0x35, 0x00)` to the panel init table, register the display with
`TEAR_AVOID_MODE_TE_SYNC`, TE on GPIO38. Writes start at the TE edge. The earlier
10.2 ms transfer estimate excluded software overhead; it does not prove that
all strips finish before the panel scan reaches them.

### 5. Moving staircase — frame-rate collapse (hardest layer)
Two stacked problems:
1. **Stock adapter waits for vsync on every 40-row strip flush**: 8 strips/frame =
   8 × 16.1 ms ≈ 130 ms/frame. Patch (`components/esp_lvgl_adapter/.../lvgl_bridge_v9.c`):
   TE_SYNC is single-buffered so flushes are strictly serial — count chunks and wait for
   vsync only on the **first strip of a frame**; the rest stream back-to-back.
2. **Strip buffers in PSRAM**: each strip costs PSRAM→PSRAM copy + byte-swap + bounce to
   internal RAM ≈ 6 ms. Switch to `use_psram=false` (now one 28.8 KB internal buffer):
   byte-swap runs in internal RAM and DMA goes direct (no bounce copy) → ~2 ms/strip.
   Whole frame: 58 ms → 35 ms (28 fps).

### 6. Speed distortion — fixed dt
TE gating makes the real frame rate float with pipeline load. Fixed dt warps motion speed
whenever the actual fps diverges. Fix: measure frame delta with `esp_timer`, clamp to
[4 ms, 50 ms].

## Debugging recipes (reusable)

- **Serial-log cadence counting**: log every N callbacks with timestamps;
  interval ÷ N = true frame period. More direct than FPS_STATS.
- **GPIO polling to prove a signal exists**: suspect TE never arrives? Tight-loop
  `gpio_get_level` for 500 ms and count rising edges (31 edges = 62 Hz confirmed).
- **Segmented timing**: wrap raster/clear/copy sections with `esp_timer_get_time()`,
  report mean + peak every 5 s.
- **Patching BSP/adapter**: vendor into `components/` (copy + remove the registry
  dependency from `idf_component.yml`). Editing `managed_components/` gets overwritten
  and pollutes other projects.

## Measured performance budget

| Stage | Time |
|---|---|
| Rasterization (incl. clear) | ~12.7 ms avg / 19.7 ms peak |
| LVGL copy + swap + DMA (internal-RAM strips) | ~16 ms |
| TE wait | ≤16.1 ms (overlaps previous frame's pipeline) |
| **Whole frame** | **~35 ms ≈ 28 fps** |

Physics: `ACC=2.4`, `DAMP=0.9`, `RESTIT=0.42`, `PMAX=0.52`, `SCALE=100 px`, `CANVAS=320`.
