#include "bot_render.h"
#include "bot_math.h"
#include <cstring>
#include "esp_heap_caps.h"

/* ------------------------------------------------ basics */

static inline uint16_t blend565(uint16_t dst, uint16_t src, float a)
{
    int dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    int sr = (src >> 11) & 31, sg = (src >> 5) & 63, sb = src & 31;
    // Round instead of truncate: removes the systematic dark bias in the 565 low bits
    int r = dr + (int)((sr - dr) * a + (sr >= dr ? 0.5f : -0.5f));
    int g = dg + (int)((sg - dg) * a + (sg >= dg ? 0.5f : -0.5f));
    int b = db + (int)((sb - db) * a + (sb >= db ? 0.5f : -0.5f));
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Coverage shaping: smoothstep softens both ends of the ramp, so edges look
 * matte instead of banded and don't shimmer in motion */
static inline float shapeCov(float c)
{
    return c * c * (3.f - 2.f * c);
}

void fb_clear(Fb &fb, uint16_t color)
{
    fb_track(fb, 0, 0, fb.w - 1, fb.h - 1);
    uint32_t pat = ((uint32_t)color << 16) | color;
    uint32_t *p = (uint32_t *)fb.px;
    size_t n = (size_t)fb.w * fb.h / 2;
    for (size_t i = 0; i < n; i++) p[i] = pat;
    if ((fb.w * fb.h) & 1) fb.px[fb.w * fb.h - 1] = color;
}

void fb_clear_rect(Fb &fb, int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb.w - 1) x1 = fb.w - 1;
    if (y1 > fb.h - 1) y1 = fb.h - 1;
    if (x0 > x1 || y0 > y1) return;
    fb_track(fb, x0, y0, x1, y1);
    uint32_t pat = ((uint32_t)color << 16) | color;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb.px + (size_t)y * fb.w;
        int x = x0;
        // Align to 32-bit boundary, then fill in 32-bit pairs (nearly 2x faster on PSRAM)
        if ((x & 1) && x <= x1) { row[x++] = color; }
        uint32_t *p32 = (uint32_t *)(row + x);
        int pairs = (x1 - x + 1) / 2;
        for (int i = 0; i < pairs; i++) p32[i] = pat;
        x += pairs * 2;
        if (x <= x1) row[x] = color;
    }
}

void fb_blend_px(Fb &fb, int x, int y, uint16_t c, float a)
{
    if (x < 0 || y < 0 || x >= fb.w || y >= fb.h || a <= 0.f) return;
    fb_track(fb, x, y, x, y);
    uint16_t &dst = fb.px[y * fb.w + x];
    dst = blend565(dst, c, a > 1.f ? 1.f : a);
}

/* ------------------------------------------------ polygon fill
 * Per-row edge buckets (each edge spans only a few rows), 4x sub-row
 * supersampling; sorted intersections pair up into coverage spans, and pixel
 * coverage = span ∩ pixel footprint length.
 */
#define MAX_ROW_EDGES 32
#define MAX_DIM 360

void fb_fill_poly(Fb &fb, const float *xs, const float *ys, int n, uint16_t color, float alpha)
{
    if (n < 3 || alpha <= 0.f || fb.h > MAX_DIM || fb.w > MAX_DIM) return;

    /* Scratch buffers live in PSRAM, allocated lazily on first use: as static
     * DRAM they cost ~24 KB, which pushed the display's 253 KB primary buffer
     * allocation over the internal-RAM limit (boot loop). */
    static int16_t (*bucket)[MAX_ROW_EDGES] = nullptr;
    static uint8_t *bcnt = nullptr;
    static float *cov = nullptr;
    if (!bucket) {
        bucket = (int16_t (*)[MAX_ROW_EDGES])heap_caps_malloc(sizeof(int16_t) * MAX_DIM * MAX_ROW_EDGES,
                                                              MALLOC_CAP_SPIRAM);
        bcnt = (uint8_t *)heap_caps_malloc(MAX_DIM, MALLOC_CAP_SPIRAM);
        cov = (float *)heap_caps_malloc(sizeof(float) * MAX_DIM, MALLOC_CAP_SPIRAM);
        if (!bucket || !bcnt || !cov) return;
    }
    memset(bcnt, 0, MAX_DIM);

    for (int i = 0; i < n; i++) {
        float y1 = ys[i], y2 = ys[(i + 1) % n];
        float ylo = y1 < y2 ? y1 : y2, yhi = y1 < y2 ? y2 : y1;
        if (yhi - ylo < 1e-6f) continue;
        int r0 = (int)floorf(ylo), r1 = (int)ceilf(yhi);
        if (r0 < 0) r0 = 0;
        if (r1 > fb.h - 1) r1 = fb.h - 1;
        for (int r = r0; r <= r1; r++) {
            if (bcnt[r] < MAX_ROW_EDGES) bucket[r][bcnt[r]++] = (int16_t)i;
        }
    }

    float hits[MAX_ROW_EDGES];
    for (int y = 0; y < fb.h; y++) {
        if (bcnt[y] == 0) continue;
        memset(cov, 0, sizeof(float) * fb.w);
        for (int sub = 0; sub < 4; sub++) {
            float yy = y + (sub + 0.5f) * 0.25f;
            int nh = 0;
            for (int e = 0; e < bcnt[y]; e++) {
                int i = bucket[y][e];
                int j = (i + 1) % n;
                float y1 = ys[i], y2 = ys[j];
                if ((y1 <= yy) != (y2 <= yy)) {
                    hits[nh++] = xs[i] + (yy - y1) * (xs[j] - xs[i]) / (y2 - y1);
                }
            }
            for (int i = 1; i < nh; i++) {  // insertion sort (hit count is tiny)
                float v = hits[i];
                int k = i - 1;
                while (k >= 0 && hits[k] > v) { hits[k + 1] = hits[k]; k--; }
                hits[k + 1] = v;
            }
            for (int i = 0; i + 1 < nh; i += 2) {
                float xa = hits[i], xb = hits[i + 1];
                int x0 = (int)floorf(xa + 0.5f) - 1, x1 = (int)ceilf(xb - 0.5f) + 1;
                if (x0 < 0) x0 = 0;
                if (x1 > fb.w - 1) x1 = fb.w - 1;
                fb_track(fb, x0, y, x1, y);
                for (int x = x0; x <= x1; x++) {
                    float lo = xa > x - 0.5f ? xa : x - 0.5f;
                    float hi = xb < x + 0.5f ? xb : x + 0.5f;
                    float c = hi - lo;
                    if (c > 0.f) cov[x] += (c > 1.f ? 1.f : c) * 0.25f;
                }
            }
        }
        uint16_t *row = fb.px + (size_t)y * fb.w;
        for (int x = 0; x < fb.w; x++) {
            if (cov[x] > 0.001f) row[x] = blend565(row[x], color, cov[x] * alpha);
        }
    }
}

/* ------------------------------------------------ ellipse fill (SDF + soft edge) */

void fb_fill_ellipse(Fb &fb, float cx, float cy, float rot, float rx, float ry, uint16_t color,
                     float alpha)
{
    if (rx < 1.f || ry < 1.f || alpha <= 0.f) return;
    float cr = cosf(rot), sr = sinf(rot);
    // Inverse transform screen -> unit-circle space: lx = (dx·cr + dy·sr)/rx, ly = (-dx·sr + dy·cr)/ry
    float invRx = 1.f / rx, invRy = 1.f / ry;
    // Bounding box (rotated ellipse's AABB, incl. the 2 px soft-edge band)
    float exx = sqrtf(rx * rx * cr * cr + ry * ry * sr * sr) + 2.5f;
    float eyy = sqrtf(rx * rx * sr * sr + ry * ry * cr * cr) + 2.5f;
    int x0 = (int)floorf(cx - exx), x1 = (int)ceilf(cx + exx);
    int y0 = (int)floorf(cy - eyy), y1 = (int)ceilf(cy + eyy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb.w - 1) x1 = fb.w - 1;
    if (y1 > fb.h - 1) y1 = fb.h - 1;
    fb_track(fb, x0, y0, x1, y1);

    // Squared distance instead of sqrtf: newlib's sqrtf is soft-float (~100 cycles), banned in hot paths
    // Soft-edge approximation: d²-1 ≈ 2(d-1) (near d≈1); pxPerUnit scales normalized distance -> pixels
    // 2 px band + smoothstep, same convention as fb_draw_ellipse_over
    float pxPerUnit = (rx + ry) * 0.5f;
    float k2 = pxPerUnit * 0.25f;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb.px + (size_t)y * fb.w;
        float dy = y + 0.5f - cy;
        // Local coords at row start, then increment per pixel to avoid per-pixel multiplies
        float lx = ((x0 + 0.5f - cx) * cr + dy * sr) * invRx;
        float ly = (-(x0 + 0.5f - cx) * sr + dy * cr) * invRy;
        float stepX = cr * invRx, stepY = -sr * invRy;
        for (int x = x0; x <= x1; x++) {
            float d2 = lx * lx + ly * ly;
            float cov = clampf(0.5f - (d2 - 1.f) * k2);
            if (cov > 0.f)
                row[x] = blend565(row[x], color, (cov >= 1.f ? 1.f : shapeCov(cov)) * alpha);
            lx += stepX;
            ly += stepY;
        }
    }
}

/* ------------------------------------------------ overwrite ellipse (writes every bbox pixel, zero reads) */

void fb_draw_ellipse_over(Fb &fb, float cx, float cy, float rot, float rx, float ry,
                          uint16_t color, uint16_t bg)
{
    if (rx < 1.f || ry < 1.f) return;
    float cr = cosf(rot), sr = sinf(rot);
    float invRx = 1.f / rx, invRy = 1.f / ry;
    float exx = sqrtf(rx * rx * cr * cr + ry * ry * sr * sr) + 2.5f;
    float eyy = sqrtf(rx * rx * sr * sr + ry * ry * cr * cr) + 2.5f;
    int x0 = (int)floorf(cx - exx), x1 = (int)ceilf(cx + exx);
    int y0 = (int)floorf(cy - eyy), y1 = (int)ceilf(cy + eyy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb.w - 1) x1 = fb.w - 1;
    if (y1 > fb.h - 1) y1 = fb.h - 1;
    fb_track(fb, x0, y0, x1, y1);

    // 2 px anti-alias band (the original 1 px band crawled during motion):
    // coverage = clamp(0.5 - pixelDist/2), then smoothstep to soften both ends
    float k2 = (rx + ry) * 0.125f;  // (d²-1)/2 ≈ d-1, times pixel scale, halved to widen the band
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb.px + (size_t)y * fb.w;
        float dy = y + 0.5f - cy;
        float lx = ((x0 + 0.5f - cx) * cr + dy * sr) * invRx;
        float ly = (-(x0 + 0.5f - cx) * sr + dy * cr) * invRy;
        float stepX = cr * invRx, stepY = -sr * invRy;
        for (int x = x0; x <= x1; x++) {
            float d2 = lx * lx + ly * ly;
            float cov = clampf(0.5f - (d2 - 1.f) * k2);
            // Every bbox pixel is written: ink inside, paper outside, edge interpolated from paper (no reads)
            row[x] = cov >= 1.f ? color : (cov <= 0.f ? bg : blend565(bg, color, shapeCov(cov)));
            lx += stepX;
            ly += stepY;
        }
    }
}

/* ------------------------------------------------ capsule (stadium) fill
 * Local half-width hw / half-height hh, mapped to screen by [a b; c d],
 * centered at (cx, cy), with a soft edge.
 */
void fb_fill_capsule(Fb &fb, float cx, float cy, float a, float b, float c, float d,
                     float hw, float hh, uint16_t color, float alpha)
{
    if (alpha <= 0.01f) return;
    float det = a * d - b * c;
    if (fabsf(det) < 1e-6f) return;
    float ia = d / det, ib = -b / det, ic = -c / det, idd = a / det;

    float exx = fabsf(a) * hw + fabsf(b) * hh + 2.5f;  // conservative bbox (incl. 2 px soft band)
    float eyy = fabsf(c) * hw + fabsf(d) * hh + 2.5f;
    int x0 = (int)floorf(cx - exx), x1 = (int)ceilf(cx + exx);
    int y0 = (int)floorf(cy - eyy), y1 = (int)ceilf(cy + eyy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb.w - 1) x1 = fb.w - 1;
    if (y1 > fb.h - 1) y1 = fb.h - 1;
    fb_track(fb, x0, y0, x1, y1);

    float r = hw < hh ? hw : hh;
    float segX = hw > r ? hw - r : 0.f;
    float segY = hh > r ? hh - r : 0.f;
    float r2 = r * r;
    // (d²-r²)/(2r) ≈ d-r: squared-distance soft edge, no hypotf; halved again for a 2 px band
    float inv2r = 1.f / (4.f * r);

    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb.px + (size_t)y * fb.w;
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float lx = ia * dx + ib * dy;
            float ly = ic * dx + idd * dy;
            float qx = clampf(lx, -segX, segX);
            float qy = clampf(ly, -segY, segY);
            float ex = lx - qx, ey = ly - qy;
            float d2 = ex * ex + ey * ey;
            float cover = clampf(0.5f - (d2 - r2) * inv2r);
            if (cover > 0.f)
                row[x] = blend565(row[x], color, (cover >= 1.f ? 1.f : shapeCov(cover)) * alpha);
        }
    }
}

/* ------------------------------------------------ color wheel + orbit arcs */

uint16_t wheel565(float hue)
{
    // bloub decor.ts wheel(): HSL wheel with S=0.55, L=0.62
    float h = fmodf(fmodf(hue, 360.f) + 360.f, 360.f);
    const float s = 0.55f, l = 0.62f;
    float c = (1.f - fabsf(2.f * l - 1.f)) * s;
    float x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f));
    float m = l - c * 0.5f;
    float r, g, b;
    if (h < 60.f)       { r = c; g = x; b = 0; }
    else if (h < 120.f) { r = x; g = c; b = 0; }
    else if (h < 180.f) { r = 0; g = c; b = x; }
    else if (h < 240.f) { r = 0; g = x; b = c; }
    else if (h < 300.f) { r = x; g = 0; b = c; }
    else                { r = c; g = 0; b = x; }
    return rgb565((uint8_t)((r + m) * 255.f), (uint8_t)((g + m) * 255.f), (uint8_t)((b + m) * 255.f));
}

void fb_arc(Fb &fb, const ArcSeed &seed, float t, float cx, float cy, float scale,
            float alpha, bool front)
{
    if (alpha <= 0.01f) return;
    const float TAU_ = 6.2831853f;
    float spin = seed.phase + t * seed.speed * TAU_;
    float cu = cosf(seed.tilt), su = sinf(seed.tilt);
    float kz = sqrtf(fmaxf(0.f, 1.f - seed.k * seed.k));
    float span = seed.sweep * TAU_;
    float r = seed.width * scale * 0.5f;      // stroke radius (pixels)
    float dotStep = r * 0.9f;                 // dot spacing < diameter keeps the stroke continuous
    float arcLen = seed.a * scale * span;     // approximate arc length (major-axis estimate)
    int n = (int)(arcLen / dotStep) + 1;
    if (n < 8) n = 8;
    if (n > 96) n = 96;

    for (int i = 0; i <= n; i++) {
        float p = (float)i / n;
        float th = spin + p * span;
        float ct = cosf(th), st = sinf(th);
        // u = (cos tilt, sin tilt, 0); v = (-sin tilt*k, cos tilt*k, kz)
        float x = seed.a * (ct * cu - st * su * seed.k) * scale + cx;
        float y = seed.a * (ct * su + st * cu * seed.k) * scale + cy;
        float z = seed.a * st * kz;
        if (front != (z >= 0.f)) continue;
        // Hue gradient along the arc (3 stops: hue / hue+span/2 / hue+span)
        uint16_t color = wheel565(seed.hue + seed.hueSpan * p);
        fb_fill_ellipse(fb, x, y, 0.f, r, r, color, alpha);  // a dot is an ellipse with rx=ry
    }
}
