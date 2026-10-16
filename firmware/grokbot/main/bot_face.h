// 面部与"生命感"：从 bloub src/bot/face.ts 移植
// 眼睛画在单位球面上（正交投影），静止时的生命全部来自目光漂移和眨眼。
#pragma once
#include "bot_math.h"

struct HeadGaze {
    float yaw;    // 度，正 = 看右
    float pitch;  // 度，正 = 看上
    float roll;   // 度，头部侧倾
};

struct EyePose {
    float x, y;          // 屏幕位置（已乘 scale）
    float a, b, c, d;    // 球面切空间 2x2 矩阵，SVG matrix(a,b,c,d,·,·) 语义
    float depth;         // 法线 z 分量，>0 = 朝前
};

constexpr float EYE_SPLIT = 15.46f;  // 双眼在球面上的半间距（度）
constexpr float EYE_W = 0.186f;      // 静止眼睛宽（球半径单位）
constexpr float EYE_H = 0.412f;      // 静止眼睛高
constexpr HeadGaze REST_GAZE = { 28.49f, 28.62f, -13.f };  // 实测静息朝向：右上方

void eyePoses(const HeadGaze &gaze, float scale, float split, EyePose out[2]);

struct Liveliness {
    float dYaw, dPitch, dRoll;
    float lid;             // 1 = 睁开，0 = 闭合
    float driftX, driftY;  // 身体微漂移（球半径单位）
    float breath;          // 呼吸缩放（仅高度）
};

Liveliness liveliness(float t, float wander = 1.f, bool blink = true, bool floating = true);

// 眨眼是屏幕坐标系里的垂直压扁（实测：宽度不变，高度落到 ~0.35）
inline float blinkScale(float lid) { return 0.06f + 0.94f * clampf(lid); }
