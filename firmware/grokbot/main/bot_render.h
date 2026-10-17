// RGB565 software rasterizer: polygon scanline fill (4x vertical supersampling AA),
// SDF ellipse/capsule fills, and 3D-projected orbit arcs.
#pragma once
#include <cstdint>
#include <algorithm>

struct PixelRect {
    int x1 = 0, y1 = 0, x2 = -1, y2 = -1;
    bool empty() const { return x1 > x2 || y1 > y2; }
    int area() const { return empty() ? 0 : (x2 - x1 + 1) * (y2 - y1 + 1); }
    void include(PixelRect other) {
        if (other.empty()) return;
        if (empty()) { *this = other; return; }
        x1 = std::min(x1, other.x1);
        y1 = std::min(y1, other.y1);
        x2 = std::max(x2, other.x2);
        y2 = std::max(y2, other.y2);
    }
};

struct Fb {
    uint16_t *px;
    int w, h;
    PixelRect *writes = nullptr;
};

inline void fb_track(Fb &fb, int x1, int y1, int x2, int y2)
{
    if (fb.writes)
        fb.writes->include({std::max(0, x1), std::max(0, y1),
                            std::min(fb.w - 1, x2), std::min(fb.h - 1, y2)});
}

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void fb_clear(Fb &fb, uint16_t color);

// Clear one rectangle only (used by the dirty-rect fast path)
void fb_clear_rect(Fb &fb, int x0, int y0, int x1, int y1, uint16_t color);
void fb_blend_px(Fb &fb, int x, int y, uint16_t c, float a);

// Closed polygon fill. xs/ys are screen pixel coords, n vertices, auto-closed.
void fb_fill_poly(Fb &fb, const float *xs, const float *ys, int n, uint16_t color, float alpha);

// Rotated ellipse fill (SDF + 2 px soft edge): center (cx,cy), half-axes rx/ry,
// rotation `rot` in radians. The body is "circle + squash", which is an ellipse
// in screen space — an order of magnitude faster than a general polygon.
void fb_fill_ellipse(Fb &fb, float cx, float cy, float rot, float rx, float ry, uint16_t color,
                     float alpha = 1.f);

// Overwrite ellipse: every pixel inside the bbox is written directly (background
// inside the box is known to be bg), so no PSRAM reads. Combined with
// "skip clear while the bbox is unchanged", idle frames cost zero clears.
void fb_draw_ellipse_over(Fb &fb, float cx, float cy, float rot, float rx, float ry,
                          uint16_t color, uint16_t bg);

// Capsule (stadium) fill: local half-width hw / half-height hh,
// mapped to screen by the 2x2 matrix [a b; c d], centered at (cx, cy), soft edge.
void fb_fill_capsule(Fb &fb, float cx, float cy, float a, float b, float c, float d,
                     float hw, float hh, uint16_t color, float alpha);

/* ------------------------------------------------ orbit rings (bloub orbit)
 * Orthographic projection of a tilted 3D circle: the circle unrolls in the plane
 * spanned by u (in-screen) and v (into the screen); the z<0 half is occluded by
 * the body. Stroked as dots along the arc, hue-graded along the sweep
 * (the wheel from bloub decor.ts).
 */
struct ArcSeed {
    float a;       // semi-major axis (ball-radius units)
    float k;       // flattening b/a (<=0.45: orbit planes seen edge-on)
    float tilt;    // screen tilt of the major axis (radians)
    float speed;   // revolutions per second
    float phase;   // initial phase
    float sweep;   // fraction of the revolution actually drawn (0..1)
    float hue;     // start hue (degrees)
    float hueSpan; // hue span along the arc (degrees)
    float width;   // stroke width (ball-radius units)
};

// front=true draws the z>=0 half (after the body), false the occluded half
// (before the body). cx/cy = screen center, scale = pixels per ball radius.
void fb_arc(Fb &fb, const ArcSeed &seed, float t, float cx, float cy, float scale,
            float alpha, bool front);

// bloub decor.ts color wheel: HSL(s=0.55, l=0.62) -> RGB565
uint16_t wheel565(float hue);
