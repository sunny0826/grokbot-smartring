// Grok Bot 重力动画 —— SmartRing-Plus (ESP32-S3-N16R8, 360x360 圆屏, QMI8658A)
//
// 形象移植自 bloub（jeremy-prt/bloub，MIT）：径向轮廓 + 球面投影眼睛。
// 重力行为：倾斜时 bot 看向低处、身体滚向低处、撞边压扁回弹；触摸 = 戳一下。
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

static const char *TAG = "grokbot";

/* ------------------------------------------------ 常量 */

#define CANVAS 320                    // 画布边长（圆屏 360，四周留边）
static constexpr float SCALE = 100.f; // 球半径（像素）
static constexpr float PMAX = 0.52f;  // 身体最大偏移（球半径单位）：(160-8-100)/100
static constexpr float ACC = 2.4f;    // 重力加速度系数：r/s² per (m/s²)
static constexpr float DAMP = 0.9f;   // 速度阻尼（1/s）
static constexpr float RESTIT = 0.42f;  // 撞边恢复系数
static const uint16_t PAPER = rgb565(0xec, 0xe7, 0xdb);  // 奶油纸色
static const uint16_t INK = rgb565(0x0a, 0x0a, 0x0c);    // bot 墨黑（bloub 实测色）

/* ------------------------------------------------ 状态 */

static Fb s_fb;
static lv_obj_t *s_canvas;

static float s_t = 0.f;              // 动画时钟（秒）
static float s_px = 0, s_py = 0;     // 身体偏移（球半径单位）
static float s_vx = 0, s_vy = 0;     // 速度
static float s_squash = 0;           // 果冻压扁量 0..1
static float s_squash_dir = 0;       // 压扁方向（弧度）
static float s_yaw = 0, s_pitch = 0, s_roll = 0;  // 平滑后的注视
static float s_mix = 0;              // 重力对注视的控制权
static float s_poke_at = -10.f;      // 上次被戳时刻
static bool s_imu_ok = false;

/* ---- 待机轮播驱动（平放静止时 thinking/notify/orbit 轮播） ---- */
static bool s_carousel = false;    // 轮播是否激活
static float s_flat = 0;           // 持续平放计时（秒）
static int s_csIdx = 0;            // CAROUSEL 当前下标
static float s_localT = 0;         // 当前状态本地时间
static BotPose s_pose;             // 当前生效姿态（morph 插值结果）
static BotPose s_prevPose;         // morph 起点快照
static bool s_prevValid = false;
static float s_morphT = 0;         // 进入当前状态以来的时间
static bool s_blinkIn = false;     // 本状态进入时是否用眨眼遮住形变

/* ------------------------------------------------ 帧回调 */

static void frame_cb(lv_timer_t *)
{
    /* 实测 dt：TE 同步把帧率门控到面板速率（可能 40~62fps 浮动），
     * 固定 dt 会在帧率波动时产生快慢失真；钳制范围防卡顿时物理跨大步 */
    static int64_t s_last_us = 0;
    int64_t nowUs = esp_timer_get_time();
    float dt = s_last_us ? (float)(nowUs - s_last_us) * 1e-6f : 1.f / 60.f;
    s_last_us = nowUs;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.004f) dt = 0.004f;
    s_t += dt;

    /* ---- 读 IMU：gx 向右、gy 向下的"下滑力"（m/s²），BSP 已做 EMA 滤波 */
    float gx = 0, gy = 0;
    if (s_imu_ok) bsp_imu_get_tilt(&gx, &gy);
    float gm = hypotf(gx, gy);

    /* ---- 物理：弹簧-阻尼滚动 + 圆形边界反弹；轮播时改为回中吸附 ---- */
    if (s_carousel) {
        // 轮播时身体居中：否则平放前球停在边缘，姿态会在偏心位置播放
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
    // 运动中的连续果冻：沿速度方向轻微拉伸
    float speed = hypotf(s_vx, s_vy);
    if (speed > 0.8f) {
        float motion = clampf((speed - 0.8f) * 0.08f, 0.f, 0.5f);
        if (motion > s_squash) {
            s_squash = motion;
            s_squash_dir = atan2f(s_vy, s_vx);
        }
    }
    s_squash *= expf(-5.f * dt);

    /* ---- 注视：重力方向 = 绝对注视目标（对应 bloub 的 setLook 语义） */
    float mixTarget = clampf((gm - 0.8f) / 2.2f);  // 近平放时让眼睛自由漂移
    s_mix += (mixTarget - s_mix) * (1.f - expf(-dt / 0.25f));
    float yawT = 38.f * clampf(gx / 9.81f, -1.f, 1.f);
    float pitchT = -38.f * clampf(gy / 9.81f, -1.f, 1.f);
    float rollT = -18.f * clampf(gx / 9.81f, -1.f, 1.f);
    float k = 1.f - expf(-dt / 0.24f);  // LOOK_MORPH = 0.24s 的追赶惯性
    s_yaw += (yawT - s_yaw) * k;
    s_pitch += (pitchT - s_pitch) * k;
    s_roll += (rollT - s_roll) * k;

    /* ---- 生命感：漂移 + 眨眼（含被戳后的人工眨眼） */
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

    /* ---- 待机轮播：近平放（gm<1.1）持续 1.5s 进入，明确倾斜（gm>2.0）立即退出。
     * 实测桌面平放基线 gm≈0.9，阈值要留裕量。
     * 切换时快照旧姿态，在新状态的 morph 时长内线性过渡；
     * blinkIn 状态用一次快速眨眼遮住形变（bloub 视频同款手法）。 */
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
        s_blinkIn = false;  // 进出轮播本身不眨眼
    }
    BotPose live;
    if (s_carousel) {
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
        // 轮播里没有重力注视，叠加完整漂移保持生命感
        live.gaze.yaw += lv.dYaw;
        live.gaze.pitch += lv.dPitch;
        live.gaze.roll += lv.dRoll;
    } else {
        poseFor(BotState::Idle, 0.f, live);
        live.gaze = gaze;  // 重力注视（含漂移）
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
        // 眨眼遮罩：morph 中点闭合最深
        float blinkLid = fabsf(2.f * morphK - 1.f);
        if (blinkLid < lid) lid = blinkLid;
    }
    gaze = { s_pose.gaze.yaw, s_pose.gaze.pitch, s_pose.gaze.roll };

    /* ---- 渲染。重力模式走脏矩形快路径；轮播有稀疏装饰物（圆环/点/徽标），
     * 它们不填满包围盒，"绘制 pass 写满盒内像素"的快路径假设不成立，整帧清屏重绘。 */
    int64_t r0 = esp_timer_get_time();

    bool decor = s_carousel || s_pose.ringA > 0.001f || s_pose.dotA[0] > 0.001f ||
                 s_pose.dotA[1] > 0.001f || s_pose.notifA > 0.001f;

    float cx0 = CANVAS / 2.f, cy0 = CANVAS / 2.f;
    float ox = s_px + lv.driftX, oy = s_py + lv.driftY;
    // squash 沿撞击/速度方向：短轴沿撞击方向，长轴垂直于它
    float sq = 1.f - 0.22f * s_squash, st = (1.f + 0.16f * s_squash) * lv.breath;
    // 姿态的身体偏移/半径叠加在重力偏移之上（轮播时重力偏移≈0）
    float rcx = cx0 + (ox + s_pose.bodyCx) * SCALE;
    float rcy = cy0 + (oy + s_pose.bodyCy) * SCALE;
    float erx = st * SCALE * s_pose.bodyR;               // 长半轴（垂直撞击方向）
    float ery = sq * SCALE * s_pose.bodyR;               // 短半轴（沿撞击方向）
    float erot = s_squash_dir + 3.14159265f / 2.f;       // 椭圆 rot 对齐长轴

    // squash 的屏幕系 2x2 变换 M = R(θ)·S(st,sq)·R(-θ)，θ = s_squash_dir
    float cr = cosf(s_squash_dir), sr = sinf(s_squash_dir);
    float m11 = st * cr * cr + sq * sr * sr;
    float m12 = cr * sr * (st - sq);
    float m22 = st * sr * sr + sq * cr * cr;

    // 包围盒（旋转椭圆外接矩形 + 3px 边距，大于绘制函数内部的 2.5px 软边垫）
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
        fb_clear(s_fb, PAPER);   // 轮播：整帧重绘
        s_first = true;          // 退回重力模式时强制全清一次
    } else {
        bool sameBox = !s_first && s_prev.x1 == cur.x1 && s_prev.y1 == cur.y1 &&
                       s_prev.x2 == cur.x2 && s_prev.y2 == cur.y2;
        if (s_first) {
            s_first = false;
            fb_clear_rect(s_fb, 0, 0, CANVAS - 1, CANVAS - 1, PAPER);
        } else if (!sameBox) {
            // 位置变了：清掉旧位置（新包围盒由绘制 pass 写满，无需清并集）
            fb_clear_rect(s_fb, s_prev.x1, s_prev.y1, s_prev.x2, s_prev.y2, PAPER);
        }
        // sameBox：包围盒没变，绘制 pass 会写满盒内每个像素，无需清屏
        s_prev = cur;
    }

    int64_t tClear = esp_timer_get_time();

    // orbit 圆环后半（z<0）：先画，被身体遮挡 —— 真深度排序让圆环读起来像轨道
    if (s_pose.ringA > 0.001f) {
        for (int i = 0; i < 6; i++) {
            float a = s_pose.ringA * clampf((s_localT - i * 0.13f) / 0.3f);
            if (a > 0.01f) fb_arc(s_fb, RING_SEEDS[i], s_localT, rcx, rcy, SCALE, a, false);
        }
    }

    fb_draw_ellipse_over(s_fb, rcx, rcy, erot, erx, ery, INK, PAPER);
    int64_t tPoly = esp_timer_get_time();

    // thinking 的两个侧点（与身体同为墨色，从球壁"长出来"）
    for (int j = 0; j < 2; j++) {
        if (s_pose.dotA[j] > 0.01f && s_pose.dotR[j] > 0.01f) {
            float dr = s_pose.dotR[j] * SCALE;
            fb_fill_ellipse(s_fb, rcx + s_pose.dotX[j] * SCALE, rcy, 0.f, dr, dr, INK,
                            s_pose.dotA[j]);
        }
    }

    // notify 徽标：纸色缺口把徽标从身体上"咬"出来，再填蓝色（NOTIF_MARGIN 跟随身体尺度）
    if (s_pose.notifA > 0.001f && s_pose.notifR > 0.005f) {
        float bx = rcx + s_pose.notifX * SCALE;
        float by = rcy + s_pose.notifY * SCALE;
        float br = s_pose.notifR * SCALE;
        float notch = br + 0.054f * SCALE * s_pose.bodyR;
        fb_fill_ellipse(s_fb, bx, by, 0.f, notch, notch, PAPER, s_pose.notifA);
        fb_fill_ellipse(s_fb, bx, by, 0.f, br, br, NOTIF_BLUE565, s_pose.notifA);
    }

    // 眼睛：球面投影 -> 胶囊。眼睛位置与矩阵都跟随身体的 squash 变换。
    if (s_pose.eyeAlpha > 0.01f) {
        EyePose ep[2];
        eyePoses(gaze, SCALE, s_pose.split, ep);
        float lidK = blinkScale(lid);
        for (int i = 0; i < 2; i++) {
            if (ep[i].depth < 0.05f) continue;  // 转到背面去了
            float alpha = clampf((ep[i].depth - 0.05f) / 0.25f) * s_pose.eyeAlpha;
            float lx = ep[i].x, ly = ep[i].y;
            float ex = rcx + m11 * lx + m12 * ly;
            float ey = rcy + m12 * lx + m22 * ly;
            // 矩阵跟随 squash，再在屏幕系垂直压扁（眨眼）
            float a = m11 * ep[i].a + m12 * ep[i].c;
            float b = m11 * ep[i].b + m12 * ep[i].d;
            float c = (m12 * ep[i].a + m22 * ep[i].c) * lidK;
            float d = (m12 * ep[i].b + m22 * ep[i].d) * lidK;
            fb_fill_capsule(s_fb, ex, ey, a, b, c, d,
                            s_pose.eyeW * SCALE / 2, s_pose.eyeH * SCALE / 2, PAPER, alpha);
        }
    }

    // orbit 圆环前半（z>=0）：盖在身体和眼睛之上
    if (s_pose.ringA > 0.001f) {
        for (int i = 0; i < 6; i++) {
            float a = s_pose.ringA * clampf((s_localT - i * 0.13f) / 0.3f);
            if (a > 0.01f) fb_arc(s_fb, RING_SEEDS[i], s_localT, rcx, rcy, SCALE, a, true);
        }
    }

    // 整幅画布失效：双缓冲条带下自维护脏矩形无法保证两份缓冲一致（拖影根因）。
    // 光栅化已优化到 ~10ms，全幅刷新在帧预算内。
    lv_obj_invalidate(s_canvas);
    s_prev = cur;

    // 渲染耗时统计：每 5 秒报一次平均/峰值
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
    if (cnt >= 300) {  // 60fps × 5s
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
        ESP_LOGI(TAG, "tilt gx=%.2f gy=%.2f | pos %.2f,%.2f | mix %.2f", gx, gy, s_px, s_py, s_mix);
    }
}

/* ---- 戳一下：给反向冲量 + 眨眼 */
static void poke_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
    if (s_t - s_poke_at < 0.3f) return;  // 防抖
    s_poke_at = s_t;
    // 逆着当前重力方向弹出去
    float gx = 0, gy = 0;
    if (s_imu_ok) bsp_imu_get_tilt(&gx, &gy);
    float gm = hypotf(gx, gy);
    float kx = gm > 0.5f ? -gx / gm : 0.6f;
    float ky = gm > 0.5f ? -gy / gm : -0.8f;
    s_vx += kx * 3.2f;
    s_vy += ky * 3.2f;
}

/* ------------------------------------------------ 入口 */

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());  // I2C + 触摸 + LCD + LVGL + 背光
    s_imu_ok = (bsp_imu_init() == ESP_OK);
    if (!s_imu_ok) ESP_LOGW(TAG, "IMU 初始化失败，重力动画降级为静止模式");

    uint16_t *buf = (uint16_t *)heap_caps_malloc(CANVAS * CANVAS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "画布缓冲分配失败");
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
    lv_obj_add_event_cb(s_canvas, poke_cb, LV_EVENT_PRESSING, NULL);

    lv_timer_create(frame_cb, 16, NULL);  // ~60 fps，匹配 TE 门控后的面板刷新率
    esp_lv_adapter_unlock();

    ESP_LOGI(TAG, "grokbot 启动完成，画布 %dx%d", CANVAS, CANVAS);
}
