// Idle-state carousel: ported from bloub src/bot/states.ts + decor.ts
// Three states — thinking (three dots) / notify (blue badge) / orbit (rings) —
// cycled as idle -> thinking -> idle -> notify -> idle -> orbit while the device
// lies flat and still. All constants are frame-measured values from bloub.
#pragma once
#include "bot_face.h"
#include "bot_render.h"

enum class BotState : int { Idle = 0, Thinking, Notify, Orbit, Sleepy, Happy, Dizzy, COUNT };
enum class PosePlayback { OneShot, Continuous };

/* Pose: parameters of every drawable element at a given instant.
 * Poses are interpolated with blendPose for morphs (bloub's morph durations). */
struct BotPose {
    // Body (ball-radius units): center offset + radius. idle/notify/orbit are
    // the r=1 ball; in thinking the ball shrinks into the middle dot (r≈0.165~0.206).
    float bodyCx, bodyCy, bodyR;
    HeadGaze gaze;
    float split;      // half eye separation on the sphere (degrees)
    float eyeW, eyeH; // eye size (ball-radius units)
    float eyeAlpha;   // 0 = faceless state (thinking)
    float eyeClose;   // 1 = eyes held shut (happy/sleepy); combined with the blink lid
    float eyeX;       // 1 = eyes drawn as X crosses (dizzy), crossfades with normal eyes
    float squashAdd;  // pose-driven extra jelly squash (happy landings), vertical
    // thinking's two side dots: x position / radius / opacity (dotA=0 = no dot)
    float dotX[2], dotR[2], dotA[2];
    // notify badge: center (ball-radius units) / radius / opacity (notifA=0 = none)
    float notifX, notifY, notifR, notifA;
    // orbit rings overall opacity (0 = no rings)
    float ringA;
    // sleepy: Zzz bubbles overall opacity (0 = none; positions animated by localT)
    float zzzA;
};

struct StateDefC {
    BotState id;
    float duration;  // hold time (seconds)
    float morph;     // entry morph duration (seconds)
    bool blinkIn;    // mask the entry morph with a blink (same trick as the bloub video)
};

// Carousel order and durations (seconds): a subset of bloub's SEQUENCE with
// idle breathers in between
extern const StateDefC CAROUSEL[];
extern const int CAROUSEL_LEN;

// Evaluate a state's pose at local time t
void poseFor(BotState s, float t, BotPose &out, PosePlayback playback = PosePlayback::OneShot);

// Linear pose interpolation (k=0 -> a, k=1 -> b)
void blendPose(BotPose &out, const BotPose &a, const BotPose &b, float k);

// orbit's 6 ring seeds (bloub decor.ts RINGS, mulberry32(0xa11ce) precomputed offline)
extern const ArcSeed RING_SEEDS[6];

// notify badge blue (measured #2496e8 in bloub)
extern const uint16_t NOTIF_BLUE565;
