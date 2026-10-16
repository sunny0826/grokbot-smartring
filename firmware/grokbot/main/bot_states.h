// 待机状态轮播：从 bloub src/bot/states.ts + decor.ts 移植
// thinking（三个点）/ notify（蓝色徽标）/ orbit（轨道圆环）三个状态，
// 设备平放静止时按 idle -> thinking -> idle -> notify -> idle -> orbit 轮播。
// 所有常数均为 bloub 对参考视频的逐帧实测值。
#pragma once
#include "bot_face.h"
#include "bot_render.h"

enum class BotState : int { Idle = 0, Thinking, Notify, Orbit, COUNT };

/* 姿态：某一帧状态下所有可绘制元素的参数。
 * 姿态间用 blendPose 做 morph 渐变（对应 bloub 的 morph 时长）。 */
struct BotPose {
    // 身体（球半径单位）：圆心偏移 + 半径。idle/notify/orbit 都是 r=1 的球；
    // thinking 里球缩成中间那个点（r≈0.165~0.206）。
    float bodyCx, bodyCy, bodyR;
    HeadGaze gaze;
    float split;      // 双眼在球面上的半间距（度）
    float eyeW, eyeH; // 眼睛尺寸（球半径单位）
    float eyeAlpha;   // 0 = 无脸状态（thinking）
    // thinking 的两个侧点：x 位置 / 半径 / 透明度（dotA=0 即无点）
    float dotX[2], dotR[2], dotA[2];
    // notify 徽标：中心（球半径单位）/ 半径 / 透明度（notifA=0 即无徽标）
    float notifX, notifY, notifR, notifA;
    // orbit 圆环整体透明度（0 = 无圆环）
    float ringA;
};

struct StateDefC {
    BotState id;
    float duration;  // 保持时长（秒）
    float morph;     // 进入 morph 时长（秒）
    bool blinkIn;    // 进入时用一次眨眼遮住形变（bloub 视频同款手法）
};

// 轮播顺序与时长（秒），bloub SEQUENCE 的子集 + idle 喘息间隔
extern const StateDefC CAROUSEL[];
extern const int CAROUSEL_LEN;

// 求某状态在本地时间 t 的姿态
void poseFor(BotState s, float t, BotPose &out);

// 姿态线性插值（k=0 取 a，k=1 取 b）
void blendPose(BotPose &out, const BotPose &a, const BotPose &b, float k);

// orbit 的 6 条轨道弧种子（bloub decor.ts RINGS，mulberry32(0xa11ce) 离线预计算）
extern const ArcSeed RING_SEEDS[6];

// notify 徽标蓝（bloub 实测 #2496e8）
extern const uint16_t NOTIF_BLUE565;
