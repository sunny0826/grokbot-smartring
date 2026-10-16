// 从 bloub src/bot/face.ts 逐行移植
#include "bot_face.h"

struct Vec3 {
    float x, y, z;
};

static constexpr float deg(float d) { return d * 3.14159265358979f / 180.f; }

// 在 u/v 张成的平面内把这对正交基旋转 angle
static void spin(Vec3 &u, Vec3 &v, float angle)
{
    float c = cosf(angle), s = sinf(angle);
    Vec3 nu = { u.x * c + v.x * s, u.y * c + v.y * s, u.z * c + v.z * s };
    Vec3 nv = { v.x * c - u.x * s, v.y * c - u.y * s, v.z * c - u.z * s };
    u = nu;
    v = nv;
}

void eyePoses(const HeadGaze &gaze, float scale, float split, EyePose out[2])
{
    Vec3 f = { 0, 0, 1 }, right = { 1, 0, 0 }, down = { 0, 1, 0 };

    spin(f, right, deg(gaze.yaw));    // 偏航：forward 倒向 right
    spin(down, f, deg(gaze.pitch));   // 俯仰：forward 倒向上（down 的反方向）
    spin(right, down, deg(gaze.roll)); // 滚转：头部在自身平面内倾斜

    for (int i = 0; i < 2; i++) {
        Vec3 ef = f, er = right;
        spin(ef, er, deg(split * (i == 0 ? -1 : 1)));
        out[i].x = ef.x * scale;
        out[i].y = ef.y * scale;
        out[i].a = er.x;
        out[i].b = er.y;
        out[i].c = down.x;
        out[i].d = down.y;
        out[i].depth = ef.z;
    }
}

/* -------------------------------------------------- 静止时的生命感 */

// 预先生成的眨眼时间表：确定性、无状态（与 bloub 同一种子）
static float BLINKS[512];
static int BLINK_COUNT = 0;

static void initBlinks()
{
    Rng rng = { 0x5eedu };
    float t = 1.4f;
    while (t < 900.f && BLINK_COUNT < 510) {
        BLINKS[BLINK_COUNT++] = t;
        t += 1.9f + rng.next() * 2.7f;
        if (rng.next() < 0.18f) {   // 偶尔双眨眼
            BLINKS[BLINK_COUNT++] = t;
            t += 0.24f;
        }
    }
}

constexpr float BLINK_DUR = 0.18f;  // 实测：10fps 下 1~2 帧

static float blinkLid(float t)
{
    // 时间表循环周期 900 秒
    t = fmodf(t, 900.f);
    for (int i = 0; i < BLINK_COUNT; i++) {
        float start = BLINKS[i];
        if (t < start) break;
        float k = (t - start) / BLINK_DUR;
        if (k >= 0.f && k <= 1.f)
            return k < 0.45f ? 1.f - k / 0.45f : (k - 0.45f) / 0.55f;  // 快闭慢开
    }
    return 1.f;
}

Liveliness liveliness(float t, float wander, bool blink, bool floating)
{
    static bool inited = false;
    if (!inited) {
        initBlinks();
        inited = true;
    }
    Liveliness l;
    // 各周期互质：漂移在肉眼看来永不重复
    l.dYaw = (loopNoise(t, 11.3f, 0.4f) * 5.5f + loopNoise(t, 3.7f, 2.1f) * 1.6f) * wander;
    l.dPitch = (loopNoise(t, 9.1f, 1.3f) * 4.2f + loopNoise(t, 4.3f, 0.7f) * 1.3f) * wander;
    l.dRoll = loopNoise(t, 13.7f, 3.2f) * 2.2f * wander;
    l.lid = blink ? blinkLid(t) : 1.f;
    l.driftX = floating ? loopNoise(t, 7.9f, 1.9f) * 0.006f : 0.f;
    l.driftY = floating ? loopNoise(t, 5.3f, 0.3f) * 0.007f : 0.f;
    l.breath = floating ? 1.f + sinf((t / 3.4f) * TAU_F) * 0.005f : 1.f;
    return l;
}
