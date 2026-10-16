// RGB565 软件光栅化器：多边形扫描线填充（4x 垂直超采样抗锯齿）+ 胶囊填充
#pragma once
#include <cstdint>

struct Fb {
    uint16_t *px;
    int w, h;
};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void fb_clear(Fb &fb, uint16_t color);

// 只清空一个矩形区域（脏矩形局部刷新用）
void fb_clear_rect(Fb &fb, int x0, int y0, int x1, int y1, uint16_t color);
void fb_blend_px(Fb &fb, int x, int y, uint16_t c, float a);

// 闭合多边形填充。xs/ys 为屏幕像素坐标，n 个顶点，首尾自动闭合。
void fb_fill_poly(Fb &fb, const float *xs, const float *ys, int n, uint16_t color, float alpha);

// 旋转椭圆填充（SDF + 2px 软边）：中心 (cx,cy)，半轴 rx/ry，旋转 rot 弧度。
// 身体是"圆 + squash"，屏幕系里就是椭圆，比通用多边形快一个量级。
void fb_fill_ellipse(Fb &fb, float cx, float cy, float rot, float rx, float ry, uint16_t color,
                     float alpha = 1.f);

// 覆盖式椭圆：包围盒内每个像素都直接写入（盒内背景已知是 bg），无 PSRAM 读操作。
// 配合"包围盒不变时跳过清屏"，静止时零清除开销。
void fb_draw_ellipse_over(Fb &fb, float cx, float cy, float rot, float rx, float ry,
                          uint16_t color, uint16_t bg);

// 胶囊（体育场形）填充：局部坐标下的半宽 hw / 半高 hh，
// 经 2x2 矩阵 [a b; c d] 映射到屏幕，中心 (cx, cy)。带 1px 软边。
void fb_fill_capsule(Fb &fb, float cx, float cy, float a, float b, float c, float d,
                     float hw, float hh, uint16_t color, float alpha);

/* ------------------------------------------------ 轨道圆环（bloub orbit）
 * 3D 倾斜圆的正交投影弧：圆在 u(屏内)/v(穿屏) 平面展开，z<0 的后半被身体遮挡。
 * 用沿弧分布的圆点描边实现，色相沿弧渐变（bloub decor.ts 的 wheel 色轮）。
 */
struct ArcSeed {
    float a;       // 半长轴（球半径单位）
    float k;       // 压扁 b/a（<=0.45，轨道平面侧视）
    float tilt;    // 长轴在屏幕上的倾角（弧度）
    float speed;   // 圈/秒
    float phase;   // 初始相位
    float sweep;   // 实际绘制的弧长比例（0..1 圈）
    float hue;     // 起始色相（度）
    float hueSpan; // 沿弧色相跨度（度）
    float width;   // 描边粗（球半径单位）
};

// front=true 画前半（z>=0，在身体之后画），false 画后半（在身体之前画）。
// cx/cy 屏幕中心，scale = 像素/球半径。
void fb_arc(Fb &fb, const ArcSeed &seed, float t, float cx, float cy, float scale,
            float alpha, bool front);

// bloub decor.ts 的色轮：HSL(s=0.55, l=0.62) -> RGB565
uint16_t wheel565(float hue);
