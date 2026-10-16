// Pose functions ported value-by-value from bloub src/bot/states.ts; constants from decor.ts.
#include "bot_states.h"
#include "bot_math.h"
#include <cmath>

static constexpr float TAU_ = 6.2831853f;

/* ---- decor.ts constants ---- */
static constexpr float DOT_X[3] = { -0.557f, -0.013f, 0.532f };  // the three dots' x (measured)
static constexpr float DOT_R = 0.165f;
static constexpr float DOT_PEAK = 1.25f;

static constexpr float NOTIF_ANGLE = -42.f * 3.14159265f / 180.f;
static constexpr float NOTIF_DIST = 1.003f;
static constexpr float NOTIF_R = 0.15f;
static constexpr float NOTIF_POP = 1.14f;

const uint16_t NOTIF_BLUE565 = rgb565(0x24, 0x96, 0xe8);

/* The 6 orbit rings, mulberry32(0xa11ce) precomputed offline (identical to bloub at runtime) */
const ArcSeed RING_SEEDS[6] = {
    { 1.374316f, 0.274937f, 0.111637f, 3.531251f, 3.521581f, 0.745288f, 4.54f, 63.33f, 0.052606f },
    { 1.347803f, 0.316944f, 1.009311f, 3.411252f, 6.146908f, 0.646808f, 81.65f, 66.52f, 0.060184f },
    { 1.396025f, 0.407338f, 1.103976f, 3.680664f, 1.062764f, 0.841260f, 149.84f, 61.37f, 0.056178f },
    { 1.385397f, 0.396188f, 1.996122f, 3.015526f, 1.116472f, 0.674048f, 185.79f, 108.99f, 0.051693f },
    { 1.375075f, 0.413304f, 2.554288f, 3.306341f, 5.341918f, 0.834125f, 269.85f, 73.29f, 0.052619f },
    { 1.303824f, 0.359854f, 3.015789f, 3.219782f, 2.674188f, 0.672155f, 327.10f, 87.63f, 0.055452f },
};

const StateDefC CAROUSEL[] = {
    { BotState::Idle,     2.4f, 0.45f, false },
    { BotState::Thinking, 2.6f, 0.40f, true  },
    { BotState::Idle,     2.0f, 0.45f, false },
    { BotState::Happy,    1.8f, 0.35f, true  },
    { BotState::Idle,     2.4f, 0.45f, false },
    { BotState::Notify,   2.2f, 0.50f, true  },
    { BotState::Idle,     2.4f, 0.45f, false },
    { BotState::Orbit,    3.4f, 0.60f, false },
    { BotState::Idle,     2.0f, 0.45f, false },
    { BotState::Sleepy,   4.2f, 0.60f, true  },
};
const int CAROUSEL_LEN = sizeof(CAROUSEL) / sizeof(CAROUSEL[0]);

/* ---- math.ts (clampf/easeOutCubic/easeInOutCubic live in bot_math.h) ---- */

/* The three-dot pulse wave: left -> middle -> right, 1.5 s period */
static float dotPulse(float t, int i)
{
    float p = fmodf(fmodf((t - i * 0.5f) / 1.5f, 1.f) + 1.f, 1.f);
    float k = p < 0.5f ? 0.5f - 0.5f * cosf(p * TAU_) : 0.f;
    return clampf(k * 2.f);
}

static void basePose(BotPose &o)
{
    o.bodyCx = 0; o.bodyCy = 0; o.bodyR = 1.f;
    o.gaze = REST_GAZE;
    o.split = EYE_SPLIT;
    o.eyeW = EYE_W; o.eyeH = EYE_H;
    o.eyeAlpha = 1.f;
    o.eyeClose = 0; o.eyeX = 0; o.squashAdd = 0;
    o.dotX[0] = o.dotX[1] = 0; o.dotR[0] = o.dotR[1] = 0; o.dotA[0] = o.dotA[1] = 0;
    o.notifX = o.notifY = 0; o.notifR = 0; o.notifA = 0;
    o.ringA = 0;
    o.zzzA = 0;
}

void poseFor(BotState s, float t, BotPose &o)
{
    basePose(o);
    switch (s) {
    case BotState::Idle:
        break;

    case BotState::Thinking: {
        // The ball itself shrinks into the middle dot; the side dots "grow out"
        // of the ball's flanks before detaching
        float mid = dotPulse(t, 1);
        float emerge = 0.3f + 0.7f * easeOutCubic(clampf(t / 0.3f));
        o.bodyR = DOT_R * (1.f + (DOT_PEAK - 1.f) * mid);
        o.bodyCx = DOT_X[1];
        o.eyeAlpha = 0.f;
        const int idx[2] = { 0, 2 };
        for (int j = 0; j < 2; j++) {
            float k = dotPulse(t, idx[j]);
            o.dotX[j] = DOT_X[idx[j]] * emerge;
            o.dotR[j] = DOT_R * (1.f + (DOT_PEAK - 1.f) * k);
            o.dotA[j] = 0.55f + 0.45f * k;
        }
        break;
    }

    case BotState::Notify: {
        // Blue badge pop: overshoots to +14% within 0.45 s, then settles;
        // the gaze points away from the badge
        float p = clampf(t / 0.45f);
        float pop = 1.f + (NOTIF_POP - 1.f) * sinf(p * 3.14159265f) * (1.f - p * 0.35f);
        float r = NOTIF_R * (p < 1.f ? pop : 1.f);
        o.gaze = { -21.94f, -5.82f, -12.2f };
        o.split = 18.89f;
        o.eyeW = 0.505f; o.eyeH = 0.498f;
        o.notifX = cosf(NOTIF_ANGLE) * NOTIF_DIST;
        o.notifY = sinf(NOTIF_ANGLE) * NOTIF_DIST;
        o.notifR = r;
        o.notifA = 1.f;
        break;
    }

    case BotState::Orbit: {
        // Simplification: the body stays a ball (bloub morphs triangle->ball over
        // the first 1.6 s; omitted). The motion is carried by the rings and the
        // gaze flying around the sphere.
        float back = easeInOutCubic(clampf((t - 1.6f) / 0.9f));
        o.gaze = {
            REST_GAZE.yaw + sinf(t * 6.5f) * 65.f * (1.f - back),
            -4.f + back * 32.f,
            -13.f
        };
        o.eyeW = 0.18f; o.eyeH = 0.34f + back * 0.07f;
        // Rings fade in over 0.8 s and out over the last 0.9 s
        // (per-ring stagger of 0.13 s is applied by the caller via ring index)
        o.ringA = clampf(t / 0.8f) * clampf((3.6f - t) / 0.9f);
        break;
    }

    case BotState::Happy: {
        // Two hops over the 1.8 s hold: body rises on a sine arc, and lands with a
        // sharp vertical squash pulse (cos^8 peaks at the arc ends)
        float k = fmodf(t, 0.9f) / 0.9f;
        o.bodyCy = -0.20f * sinf(k * 3.14159265f);
        float cs = cosf(k * 3.14159265f);
        float cs2 = cs * cs, cs4 = cs2 * cs2;
        o.squashAdd = 0.4f * cs4 * cs4;
        o.eyeClose = 1.f;  // joy = eyes shut
        o.gaze = { REST_GAZE.yaw * 0.3f, 18.f, 0.f };
        break;
    }

    case BotState::Sleepy: {
        // Sag down, eyes shut, Zzz bubbles float off the upper right
        // (positions animated by the renderer from localT)
        o.eyeClose = 1.f;
        o.bodyCy = 0.06f;
        o.bodyR = 0.97f;
        o.gaze = { 0.f, -22.f, -6.f };
        o.zzzA = clampf((t - 0.3f) / 0.5f);
        break;
    }

    case BotState::Dizzy: {
        // Shake reaction (not part of the carousel): X eyes, spiralling gaze,
        // wobbling body. Driven directly by main.cpp while the dizzy timer runs.
        o.gaze = { sinf(t * 9.f) * 55.f, cosf(t * 7.f) * 30.f - 10.f, sinf(t * 11.f) * 15.f };
        o.eyeX = 1.f;
        o.bodyCx = sinf(t * 6.3f) * 0.05f;
        o.bodyCy = cosf(t * 5.1f) * 0.04f;
        break;
    }
    default: break;
    }
}

static inline float lr(float a, float b, float k) { return a + (b - a) * k; }

void blendPose(BotPose &o, const BotPose &a, const BotPose &b, float k)
{
    o.bodyCx = lr(a.bodyCx, b.bodyCx, k);
    o.bodyCy = lr(a.bodyCy, b.bodyCy, k);
    o.bodyR  = lr(a.bodyR, b.bodyR, k);
    o.gaze.yaw   = lr(a.gaze.yaw, b.gaze.yaw, k);
    o.gaze.pitch = lr(a.gaze.pitch, b.gaze.pitch, k);
    o.gaze.roll  = lr(a.gaze.roll, b.gaze.roll, k);
    o.split = lr(a.split, b.split, k);
    o.eyeW  = lr(a.eyeW, b.eyeW, k);
    o.eyeH  = lr(a.eyeH, b.eyeH, k);
    o.eyeAlpha = lr(a.eyeAlpha, b.eyeAlpha, k);
    o.eyeClose = lr(a.eyeClose, b.eyeClose, k);
    o.eyeX = lr(a.eyeX, b.eyeX, k);
    o.squashAdd = lr(a.squashAdd, b.squashAdd, k);
    for (int i = 0; i < 2; i++) {
        o.dotX[i] = lr(a.dotX[i], b.dotX[i], k);
        o.dotR[i] = lr(a.dotR[i], b.dotR[i], k);
        o.dotA[i] = lr(a.dotA[i], b.dotA[i], k);
    }
    o.notifX = lr(a.notifX, b.notifX, k);
    o.notifY = lr(a.notifY, b.notifY, k);
    o.notifR = lr(a.notifR, b.notifR, k);
    o.notifA = lr(a.notifA, b.notifA, k);
    o.ringA  = lr(a.ringA, b.ringA, k);
    o.zzzA   = lr(a.zzzA, b.zzzA, k);
}
