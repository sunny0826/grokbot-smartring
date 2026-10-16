// Face and "liveliness": ported from bloub src/bot/face.ts
// Eyes live on the unit sphere (orthographic projection); at rest, all life
// comes from gaze drift and blinking.
#pragma once
#include "bot_math.h"

struct HeadGaze {
    float yaw;    // degrees, positive = look right
    float pitch;  // degrees, positive = look up
    float roll;   // degrees, head tilt
};

struct EyePose {
    float x, y;          // screen position (already scaled)
    float a, b, c, d;    // sphere tangent-space 2x2 matrix, SVG matrix(a,b,c,d,·,·) semantics
    float depth;         // normal z component, >0 = facing forward
};

constexpr float EYE_SPLIT = 15.46f;  // half eye separation on the sphere (degrees)
constexpr float EYE_W = 0.186f;      // resting eye width (ball-radius units)
constexpr float EYE_H = 0.412f;      // resting eye height
constexpr HeadGaze REST_GAZE = { 28.49f, 28.62f, -13.f };  // measured resting gaze: up-right

void eyePoses(const HeadGaze &gaze, float scale, float split, EyePose out[2]);

struct Liveliness {
    float dYaw, dPitch, dRoll;
    float lid;             // 1 = open, 0 = closed
    float driftX, driftY;  // subtle body drift (ball-radius units)
    float breath;          // breathing scale (height only)
};

Liveliness liveliness(float t, float wander = 1.f, bool blink = true, bool floating = true);

// Blinking is a vertical squash in screen space (measured: width unchanged, height drops to ~0.35)
inline float blinkScale(float lid) { return 0.06f + 0.94f * clampf(lid); }
