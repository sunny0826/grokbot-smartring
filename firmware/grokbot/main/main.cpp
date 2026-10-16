// Grok Bot gravity animation — SmartRing-Plus (ESP32-S3-N16R8, 360x360 round panel, QMI8658A)
//
// Character ported from bloub (jeremy-prt/bloub, MIT): radial outline + spherically projected eyes.
// Gravity behavior: when tilted, the bot looks downhill, its body rolls downhill, and it squashes
// and bounces on edge impact; touch = poke.
#include <cmath>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/smartring_plus.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

#include "bot_math.h"
#include "bot_face.h"
#include "bot_render.h"
#include "bot_states.h"
#include "bot_character.h"

static const char *TAG = "grokbot";

/* ------------------------------------------------ Constants */

#define CANVAS 320                    // canvas side length (360 round panel with margin)
static constexpr float SCALE = 100.f; // ball radius (pixels)
static constexpr float PMAX = 0.52f;  // max body offset (in ball radii): (160-8-100)/100
static constexpr float ACC = 2.4f;    // gravity gain: r/s^2 per (m/s^2)
static constexpr float DAMP = 0.9f;   // velocity damping (1/s)
static constexpr float RESTIT = 0.42f;  // edge restitution
static const uint16_t PAPER = rgb565(0xec, 0xe7, 0xdb);  // cream paper color
static const uint16_t INK = rgb565(0x0a, 0x0a, 0x0c);    // bot ink black (measured from bloub)

/* ------------------------------------------------ State */

static Fb s_fb;
static lv_obj_t *s_canvas;

static float s_t = 0.f;              // animation clock (seconds)
static float s_px = 0, s_py = 0;     // body offset (in ball radii)
static float s_vx = 0, s_vy = 0;     // velocity
static float s_squash = 0;           // jelly squash amount 0..1
static float s_squash_dir = 0;       // squash direction (radians)
static float s_yaw = 0, s_pitch = 0, s_roll = 0;  // smoothed gaze
static float s_mix = 0;              // how much gravity controls the gaze
static float s_poke_at = -10.f;      // time of last poke
static bool s_imu_ok = false;
static bool s_force_clear = false;   // full clear on next gravity frame (set on skin switch)
static float s_ux = 0.f, s_uy = -1.f;  // smoothed world-up direction (screen coords)

/* ---- Touch: tap/hold = poke, horizontal swipe = switch character skin ---- */
static lv_point_t s_tp0;             // press start (screen coords)
static bool s_tactive = false;
static bool s_tswiped = false;
static float s_tpress_at = 0.f;

/* ---- Idle carousel driver (cycles thinking/notify/orbit when lying flat) ---- */
static bool s_carousel = false;    // carousel active
static float s_flat = 0;           // continuous flat-lying timer (seconds)
static int s_csIdx = 0;            // current CAROUSEL index
static float s_localT = 0;         // local time in current state
static BotPose s_pose;             // active pose (morph interpolation result)
static BotPose s_prevPose;         // morph start snapshot
static bool s_prevValid = false;
static float s_morphT = 0;         // time since entering current state
static bool s_blinkIn = false;     // mask the transition with a blink on entry

/* ---- Shake -> dizzy: high-pass on the tilt vector (jerk), with a cooldown ---- */
static float s_pgx = 0, s_pgy = 0;   // previous frame tilt
static float s_jerk = 0;             // smoothed tilt jerk (m/s^2 per s)
static float s_dizzy = 0;            // remaining dizzy time (seconds)
static float s_dizzyT = 0;           // local time inside dizzy
static float s_lastDizzy = -10.f;    // last dizzy trigger (cooldown)

/* ------------------------------------------------ Frame callback */

static void frame_cb(lv_timer_t *)
{
    /* Measured dt: TE sync gates the frame rate to the panel rate (may drift 40~62fps);
     * a fixed dt would distort speed when the rate fluctuates; the clamp prevents huge
     * physics steps after a stall. */
    static int64_t s_last_us = 0;
    int64_t nowUs = esp_timer_get_time();
    float dt = s_last_us ? (float)(nowUs - s_last_us) * 1e-6f : 1.f / 60.f;
    s_last_us = nowUs;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.004f) dt = 0.004f;
    s_t += dt;

    /* ---- Read IMU: "downhill force" with gx right, gy down (m/s^2); the BSP applies EMA filtering */
    float gx = 0, gy = 0;
    if (s_imu_ok) bsp_imu_get_tilt(&gx, &gy);
    float gm = hypotf(gx, gy);

    /* ---- Shake detection: jerk on the (EMA-filtered) tilt vector. The BSP filter
     * smooths slow motion, so only a real shake pushes the jerk over the threshold. */
    float jerk = hypotf(gx - s_pgx, gy - s_pgy) / dt;
    s_pgx = gx;
    s_pgy = gy;
    s_jerk += (jerk - s_jerk) * (1.f - expf(-dt / 0.15f));
    if (s_dizzy <= 0.f && s_jerk > 30.f && s_t - s_lastDizzy > 5.f) {
        s_dizzy = 1.8f;
        s_dizzyT = 0.f;
        s_lastDizzy = s_t;
        s_prevPose = s_pose;  // morph into the dizzy pose
        s_prevValid = true;
        s_morphT = 0;
        ESP_LOGI(TAG, "shake! dizzy (jerk %.1f)", s_jerk);
    }

    /* ---- Physics: spring-damped rolling + circular boundary bounce; carousel pulls back to center ---- */
    if (s_carousel) {
        // Center the body during carousel: otherwise the ball rests at the edge when laid flat
        // and poses would play off-center.
        float ck = 1.f - expf(-dt / 0.3f);
        s_px -= s_px * ck;
        s_py -= s_py * ck;
        s_vx = s_vy = 0;
    } else {
        s_vx += gx * ACC * dt;
        s_vy += gy * ACC * dt;
    }
    float damp = expf(-DAMP * dt);
    s_vx *= damp;
    s_vy *= damp;
    s_px += s_vx * dt;
    s_py += s_vy * dt;

    float pm = hypotf(s_px, s_py);
    if (pm > PMAX) {
        float nx = s_px / pm, ny = s_py / pm;
        s_px = nx * PMAX;
        s_py = ny * PMAX;
        float vn = s_vx * nx + s_vy * ny;
        if (vn > 0.f) {
            s_vx -= (1.f + RESTIT) * vn * nx;
            s_vy -= (1.f + RESTIT) * vn * ny;
            float impact = clampf(vn * 0.3f);
            if (impact > s_squash) {
                s_squash = impact;
                s_squash_dir = atan2f(ny, nx);
            }
        }
    }
    // Continuous jelly while moving: slight stretch along the velocity direction
    float speed = hypotf(s_vx, s_vy);
    if (speed > 0.8f) {
        float motion = clampf((speed - 0.8f) * 0.08f, 0.f, 0.5f);
        if (motion > s_squash) {
            s_squash = motion;
            s_squash_dir = atan2f(s_vy, s_vx);
        }
    }
    s_squash *= expf(-5.f * dt);

    /* ---- Gaze: gravity direction = absolute gaze target (bloub setLook semantics) */
    float mixTarget = clampf((gm - 0.8f) / 2.2f);  // let the eyes wander freely near flat
    s_mix += (mixTarget - s_mix) * (1.f - expf(-dt / 0.25f));
    float yawT = 38.f * clampf(gx / 9.81f, -1.f, 1.f);
    float pitchT = -38.f * clampf(gy / 9.81f, -1.f, 1.f);
    float rollT = -18.f * clampf(gx / 9.81f, -1.f, 1.f);
    float k = 1.f - expf(-dt / 0.24f);  // LOOK_MORPH = 0.24s chase inertia
    s_yaw += (yawT - s_yaw) * k;
    s_pitch += (pitchT - s_pitch) * k;
    s_roll += (rollT - s_roll) * k;

    /* ---- Liveliness: wander + blink (including the artificial blink after a poke) */
    float wander = 1.f - 0.7f * s_mix;
    Liveliness lv = liveliness(s_t, wander, true, true);
    float lid = lv.lid;
    float pk = (s_t - s_poke_at) / 0.18f;
    if (pk >= 0.f && pk <= 1.f) {
        float pokeLid = pk < 0.45f ? 1.f - pk / 0.45f : (pk - 0.45f) / 0.55f;
        if (pokeLid < lid) lid = pokeLid;
    }

    HeadGaze gaze = {
        lerpf(REST_GAZE.yaw + lv.dYaw, s_yaw, s_mix),
        lerpf(REST_GAZE.pitch + lv.dPitch, s_pitch, s_mix),
        lerpf(REST_GAZE.roll + lv.dRoll, s_roll, s_mix),
    };

    /* ---- Upright rotation: world-up direction in screen coords, smoothed as a vector
     * (angle smoothing would snap across ±180°). Skins that keep their head upright
     * (pika's ears) rotate their local frame by (upCos, upSin); grokbot ignores it.
     * Near flat the target eases back to screen-up, so the carousel stays upright. */
    float tux = 0.f, tuy = -1.f;
    if (gm > 0.8f) { tux = -gx / gm; tuy = -gy / gm; }
    float uk = 1.f - expf(-dt / 0.2f);
    s_ux += (tux - s_ux) * uk;
    s_uy += (tuy - s_uy) * uk;
    float um = hypotf(s_ux, s_uy);
    float upCos = 1.f, upSin = 0.f;
    if (um > 1e-3f) {
        // R(up) maps local up (0,-1) to (upSin, -upCos) = normalized world-up
        upSin = s_ux / um;
        upCos = -s_uy / um;
    }

    /* ---- Idle carousel: enter after lying near-flat (gm<1.1) for 1.5s, exit immediately on a
     * clear tilt (gm>2.0). The desk-flat baseline measures gm~=0.9, so thresholds need margin.
     * On switching, snapshot the old pose and blend linearly over the new state's morph time;
     * blinkIn states hide the deformation with a quick blink (same trick as the bloub video). */
    if (gm > 2.0f) s_flat = 0;
    else if (gm < 1.1f) s_flat += dt;
    bool wantCarousel = s_flat > 1.5f;
    if (wantCarousel != s_carousel) {
        s_prevPose = s_pose;
        s_prevValid = true;
        s_morphT = 0;
        s_carousel = wantCarousel;
        s_csIdx = 0;
        s_localT = 0;
        s_blinkIn = false;  // entering/leaving the carousel itself does not blink
    }
    BotPose live;
    if (s_dizzy > 0.f) {
        // Shake reaction overrides everything; the body keeps rolling physically
        s_dizzy -= dt;
        s_dizzyT += dt;
        poseFor(BotState::Dizzy, s_dizzyT, live);
        if (s_dizzy <= 0.f) {
            s_prevPose = s_pose;  // morph back out
            s_prevValid = true;
            s_morphT = 0;
        }
    } else if (s_carousel) {
        s_localT += dt;
        const StateDefC &def = CAROUSEL[s_csIdx];
        if (s_localT >= def.duration) {
            s_prevPose = s_pose;
            s_prevValid = true;
            s_morphT = 0;
            s_csIdx = (s_csIdx + 1) % CAROUSEL_LEN;
            s_localT = 0;
            s_blinkIn = CAROUSEL[s_csIdx].blinkIn;
        }
        poseFor(CAROUSEL[s_csIdx].id, s_localT, live);
        // No gravity gaze in the carousel; add the full wander to keep it alive
        live.gaze.yaw += lv.dYaw;
        live.gaze.pitch += lv.dPitch;
        live.gaze.roll += lv.dRoll;
    } else {
        poseFor(BotState::Idle, 0.f, live);
        live.gaze = gaze;  // gravity gaze (including wander)
    }
    s_morphT += dt;
    float morphK = s_prevValid ? clampf(s_morphT / CAROUSEL[s_csIdx].morph) : 1.f;
    if (morphK < 1.f) {
        blendPose(s_pose, s_prevPose, live, morphK);
    } else {
        s_pose = live;
        s_prevValid = false;
    }
    if (s_blinkIn && morphK < 1.f) {
        // Blink mask: deepest closure at the morph midpoint
        float blinkLid = fabsf(2.f * morphK - 1.f);
        if (blinkLid < lid) lid = blinkLid;
    }
    // Pose-driven eye closure (happy/sleepy) stacks on top of the blink lid
    lid = fminf(lid, 1.f - s_pose.eyeClose);
    gaze = { s_pose.gaze.yaw, s_pose.gaze.pitch, s_pose.gaze.roll };

    /* ---- Render. Gravity mode takes the dirty-rect fast path; the carousel has sparse
     * decorations (ring/dots/badge) that do not fill the bounding box, so the fast-path
     * assumption "the draw pass fills every pixel in the box" breaks — clear and redraw fully. */
    int64_t r0 = esp_timer_get_time();

    bool decor = s_carousel || s_pose.ringA > 0.001f || s_pose.dotA[0] > 0.001f ||
                 s_pose.dotA[1] > 0.001f || s_pose.notifA > 0.001f || character_full_clear();

    float cx0 = CANVAS / 2.f, cy0 = CANVAS / 2.f;
    float ox = s_px + lv.driftX, oy = s_py + lv.driftY;
    // Squash = impact/velocity squash + pose-driven squash (happy landings; vertical)
    float squashAll = s_squash + s_pose.squashAdd;
    float squashDir = s_squash > 0.03f ? s_squash_dir : 1.5707963f;
    // Squash along the impact/velocity direction: short axis along impact, long axis perpendicular
    float sq = 1.f - 0.22f * squashAll, st = (1.f + 0.16f * squashAll) * lv.breath;
    // Pose body offset/radius stack on top of the gravity offset (~0 during carousel)
    float rcx = cx0 + (ox + s_pose.bodyCx) * SCALE;
    float rcy = cy0 + (oy + s_pose.bodyCy) * SCALE;
    float erx = st * SCALE * s_pose.bodyR;               // long semi-axis (perpendicular to impact)
    float ery = sq * SCALE * s_pose.bodyR;               // short semi-axis (along impact)
    float erot = squashDir + 3.14159265f / 2.f;          // ellipse rot aligns with the long axis

    // Squash as a screen-space 2x2 transform M = R(theta)·S(st,sq)·R(-theta), theta = squashDir
    float cr = cosf(squashDir), sr = sinf(squashDir);
    float m11 = st * cr * cr + sq * sr * sr;
    float m12 = cr * sr * (st - sq);
    float m22 = st * sr * sr + sq * cr * cr;

    // Bounding box (rotated-ellipse AABB + 3px margin, larger than the 2.5px soft-edge pad inside the draw calls)
    float bc = cosf(erot), bs = sinf(erot);
    float bex = sqrtf(erx * erx * bc * bc + ery * ery * bs * bs);
    float bey = sqrtf(erx * erx * bs * bs + ery * ery * bc * bc);
    lv_area_t cur = {
        (lv_coord_t)(floorf(rcx - bex) - 3), (lv_coord_t)(floorf(rcy - bey) - 3),
        (lv_coord_t)(ceilf(rcx + bex) + 3),  (lv_coord_t)(ceilf(rcy + bey) + 3)
    };
    if (cur.x1 < 0) cur.x1 = 0;
    if (cur.y1 < 0) cur.y1 = 0;
    if (cur.x2 > CANVAS - 1) cur.x2 = CANVAS - 1;
    if (cur.y2 > CANVAS - 1) cur.y2 = CANVAS - 1;

    static lv_area_t s_prev = { 0, 0, 0, 0 };
    static bool s_first = true;
    if (decor) {
        fb_clear(s_fb, PAPER);   // carousel: full redraw
        s_first = true;          // force one full clear when returning to gravity mode
    } else {
        bool sameBox = !s_first && s_prev.x1 == cur.x1 && s_prev.y1 == cur.y1 &&
                       s_prev.x2 == cur.x2 && s_prev.y2 == cur.y2;
        if (s_first || s_force_clear) {
            s_first = false;
            s_force_clear = false;
            fb_clear_rect(s_fb, 0, 0, CANVAS - 1, CANVAS - 1, PAPER);
        } else if (!sameBox) {
            // Position changed: clear the old spot (the new box is fully written by the draw pass,
            // no need to clear the union)
            fb_clear_rect(s_fb, s_prev.x1, s_prev.y1, s_prev.x2, s_prev.y2, PAPER);
        }
        // sameBox: box unchanged and the draw pass writes every pixel in it — no clear needed
        s_prev = cur;
    }

    int64_t tClear = esp_timer_get_time();

    // The character itself lives in bot_character.cpp (the "skin"); everything it
    // needs is resolved here into plain numbers.
    CharacterDrawParams cp = {
        s_fb,
        rcx, rcy,
        erot, erx, ery,
        m11, m12, m22,
        gaze,
        &s_pose,
        lid,
        SCALE,
        s_localT,
        INK, PAPER,
        upCos, upSin,
    };
    character_draw(cp);
    int64_t tPoly = esp_timer_get_time();

    // Invalidate the whole canvas: with double-buffered draw strips, self-maintained dirty rects
    // cannot keep both buffers consistent (root cause of ghosting).
    // Rasterization is optimized to ~10ms, so a full-area flush fits the frame budget.
    lv_obj_invalidate(s_canvas);
    s_prev = cur;

    // Render timing stats: report average/peak every 5 seconds
    int64_t rUs = esp_timer_get_time() - r0;
    static int64_t acc = 0, peak = 0, accPoly = 0, accClear = 0, lastTick = 0, gapPeak = 0;
    static int cnt = 0;
    acc += rUs;
    accPoly += tPoly - tClear;
    accClear += tClear - r0;
    if (lastTick && r0 - lastTick > gapPeak) gapPeak = r0 - lastTick;
    lastTick = r0;
    if (rUs > peak) peak = rUs;
    cnt++;
    if (cnt >= 300) {  // 60fps x 5s
        ESP_LOGI(TAG, "raster avg=%lldus(clear %lld + poly %lld + rest) peak=%lldus | frame gap peak=%lldus | box=%dx%d",
                 acc / cnt, accClear / cnt, accPoly / cnt, peak, gapPeak,
                 cur.x2 - cur.x1 + 1, cur.y2 - cur.y1 + 1);
        acc = 0;
        accPoly = 0;
        accClear = 0;
        peak = 0;
        gapPeak = 0;
        cnt = 0;
    }

    static int logDiv = 0;
    if (++logDiv >= 60) {
        logDiv = 0;
        ESP_LOGI(TAG, "tilt gx=%.2f gy=%.2f | pos %.2f,%.2f | mix %.2f | jerk %.1f", gx, gy, s_px,
                 s_py, s_mix, s_jerk);
    }
}

/* ---- Poke: impulse away from gravity + a blink */
static void do_poke()
{
    if (s_t - s_poke_at < 0.3f) return;  // debounce
    s_poke_at = s_t;
    // Fling away from the current gravity direction
    float gx = 0, gy = 0;
    if (s_imu_ok) bsp_imu_get_tilt(&gx, &gy);
    float gm = hypotf(gx, gy);
    float kx = gm > 0.5f ? -gx / gm : 0.6f;
    float ky = gm > 0.5f ? -gy / gm : -0.8f;
    s_vx += kx * 3.2f;
    s_vy += ky * 3.2f;
}

/* ---- Touch handler: tap or stationary hold = poke; horizontal swipe = next/prev skin.
 * A press only pokes once it has stayed within a 40 px radius for 0.12 s (a quick tap
 * pokes on release), so the start of a swipe never triggers a spurious poke. */
static void touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_tp0 = pt;
        s_tactive = true;
        s_tswiped = false;
        s_tpress_at = s_t;
        return;
    }
    if (!s_tactive) return;
    float dx = (float)(pt.x - s_tp0.x), dy = (float)(pt.y - s_tp0.y);

    if (code == LV_EVENT_PRESSING) {
        if (fabsf(dx) > 40.f || fabsf(dy) > 40.f) s_tswiped = true;
        if (!s_tswiped && s_t - s_tpress_at > 0.12f) do_poke();
    } else if (code == LV_EVENT_RELEASED) {
        s_tactive = false;
        if (s_tswiped && fabsf(dx) > 60.f && fabsf(dx) > fabsf(dy) * 1.2f) {
            character_select(character_index() + (dx > 0 ? 1 : -1));
            s_force_clear = true;  // the new skin may not fill the old one's pixels
            ESP_LOGI(TAG, "character -> %s (%d/%d)", character_name(character_index()),
                     character_index() + 1, character_count());
        } else if (!s_tswiped) {
            do_poke();  // quick tap, shorter than the hold threshold
        }
    }
}

/* ------------------------------------------------ Entry point */

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());  // I2C + touch + LCD + LVGL + backlight
    s_imu_ok = (bsp_imu_init() == ESP_OK);
    if (!s_imu_ok) ESP_LOGW(TAG, "IMU init failed, gravity animation degraded to static mode");

    uint16_t *buf = (uint16_t *)heap_caps_malloc(CANVAS * CANVAS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "canvas buffer alloc failed");
        return;
    }
    s_fb = { buf, CANVAS, CANVAS };

    esp_lv_adapter_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xece7db), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, buf, CANVAS, CANVAS, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_RELEASED, NULL);
    // The 20 px ring outside the canvas still belongs to the screen: swipes may start there
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_RELEASED, NULL);

    lv_timer_create(frame_cb, 16, NULL);  // ~60 fps, matching the TE-gated panel refresh rate
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "grokbot started, canvas %dx%d, %d characters (swipe to switch)", CANVAS, CANVAS,
             character_count());
}
