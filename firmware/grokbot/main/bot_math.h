// Math utilities: ported from bloub (jeremy-prt/bloub, MIT) src/bot/math.ts
#pragma once
#include <cmath>
#include <cstdint>

constexpr float TAU_F = 6.283185307179586f;

inline float clampf(float v, float lo = 0.f, float hi = 1.f) { return v < lo ? lo : v > hi ? hi : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Measured on the reference video: transitions are exponential ease-outs, the body never overshoots
inline float easeOutCubic(float t) { float u = 1.f - t; return 1.f - u * u * u; }
inline float easeInOutCubic(float t) { return t < 0.5f ? 4.f * t * t * t : 1.f - powf(-2.f * t + 2.f, 3) / 2.f; }
inline float easeOutQuint(float t) { float u = 1.f - t; return 1.f - u * u * u * u * u; }

// Periodic 1D noise: seamless loop over `period`, used for gaze drift
inline float loopNoise(float t, float period, float seed = 0.f)
{
    float p = (t / period) * TAU_F;
    return 0.55f * sinf(p + seed) + 0.3f * sinf(2.f * p + seed * 1.7f + 1.1f) + 0.15f * sinf(3.f * p + seed * 2.3f + 2.4f);
}

// mulberry32, deterministic PRNG (same algorithm as bloub, so blink schedules match)
struct Rng {
    uint32_t a;
    float next()
    {
        a += 0x6d2b79f5u;
        uint32_t t = (a ^ (a >> 15)) * (1u | a);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return (float)(t ^ (t >> 14)) / 4294967296.0f;
    }
};

// Catmull-Rom interpolation, upsamples the 64-point radial contour to arbitrary precision
inline float catmullRom(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t * t +
                   (-p0 + 3.f * p1 - 3.f * p2 + p3) * t * t * t);
}
