// 数学工具：从 bloub (jeremy-prt/bloub, MIT) 的 src/bot/math.ts 移植
#pragma once
#include <cmath>
#include <cstdint>

constexpr float TAU_F = 6.283185307179586f;

inline float clampf(float v, float lo = 0.f, float hi = 1.f) { return v < lo ? lo : v > hi ? hi : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// 实测：状态过渡是指数缓出，身体永不过冲
inline float easeOutCubic(float t) { float u = 1.f - t; return 1.f - u * u * u; }
inline float easeInOutCubic(float t) { return t < 0.5f ? 4.f * t * t * t : 1.f - powf(-2.f * t + 2.f, 3) / 2.f; }
inline float easeOutQuint(float t) { float u = 1.f - t; return 1.f - u * u * u * u * u; }

// 周期 1D 噪声：无缝循环，用于目光漂移
inline float loopNoise(float t, float period, float seed = 0.f)
{
    float p = (t / period) * TAU_F;
    return 0.55f * sinf(p + seed) + 0.3f * sinf(2.f * p + seed * 1.7f + 1.1f) + 0.15f * sinf(3.f * p + seed * 2.3f + 2.4f);
}

// mulberry32，确定性 PRNG（与 bloub 同一算法，眨眼时间表一致）
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

// Catmull-Rom 插值，用于把 64 采样径向轮廓加密到任意精度
inline float catmullRom(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t * t +
                   (-p0 + 3.f * p1 - 3.f * p2 + p3) * t * t * t);
}
