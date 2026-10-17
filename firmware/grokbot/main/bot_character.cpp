// Character skins: registry + the default grok-bot skin (ported from bloub,
// jeremy-prt/bloub, MIT). Draw order back rings -> body -> dots -> badge ->
// eyes -> front rings yields real depth ordering for the orbit.
#include "bot_character.h"
#include <cmath>

// Skins living in their own translation units:
void pika_draw(CharacterDrawParams &p);    // bot_character_pika.cpp
void rowlet_draw(CharacterDrawParams &p);  // bot_character_rowlet.cpp

/* ---------------- shared building blocks ---------------- */

void skin_draw_rings_back(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    if (pose.ringA > 0.001f) {
        for (int i = 0; i < 6; i++) {
            float a = pose.ringA * clampf((p.localT - i * 0.13f) / 0.3f);
            if (a > 0.01f) fb_arc(p.fb, RING_SEEDS[i], p.localT, p.rcx, p.rcy, p.scale, a, false);
        }
    }
}

void skin_draw_rings_front(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    if (pose.ringA > 0.001f) {
        for (int i = 0; i < 6; i++) {
            float a = pose.ringA * clampf((p.localT - i * 0.13f) / 0.3f);
            if (a > 0.01f) fb_arc(p.fb, RING_SEEDS[i], p.localT, p.rcx, p.rcy, p.scale, a, true);
        }
    }
}

void skin_draw_dots(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    // The two thinking side dots (same ink as the body, "growing" out of the ball wall)
    for (int j = 0; j < 2; j++) {
        if (pose.dotA[j] > 0.01f && pose.dotR[j] > 0.01f) {
            float dr = pose.dotR[j] * p.scale;
            fb_fill_ellipse(p.fb, p.rcx + pose.dotX[j] * p.scale, p.rcy, 0.f, dr, dr, p.ink,
                            pose.dotA[j]);
        }
    }
}

void skin_draw_badge(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    // Notify badge: a paper-colored notch "bites" the badge out of the body, then it is
    // filled blue (the 0.054 margin follows the body scale)
    if (pose.notifA > 0.001f && pose.notifR > 0.005f) {
        float bx = p.rcx + pose.notifX * p.scale;
        float by = p.rcy + pose.notifY * p.scale;
        float br = pose.notifR * p.scale;
        float notch = br + 0.054f * p.scale * pose.bodyR;
        fb_fill_ellipse(p.fb, bx, by, 0.f, notch, notch, p.paper, pose.notifA);
        fb_fill_ellipse(p.fb, bx, by, 0.f, br, br, NOTIF_BLUE565, pose.notifA);
    }
}

void skin_draw_eyes(CharacterDrawParams &p, float t11, float t12, float t21, float t22,
                    float splitDeg, float eyeW, float eyeH, uint16_t color565, float elDeg)
{
    const BotPose &pose = *p.pose;
    if (pose.eyeAlpha <= 0.01f) return;
    float eyeX = clampf(pose.eyeX);
    EyePose ep[2];
    eyePoses(p.gaze, p.scale, splitDeg, ep, elDeg);
    float lidK = blinkScale(p.lid);
    for (int i = 0; i < 2; i++) {
        if (ep[i].depth < 0.05f) continue;  // rotated to the back
        float alpha = clampf((ep[i].depth - 0.05f) / 0.25f) * pose.eyeAlpha;
        float lx = ep[i].x, ly = ep[i].y;
        float ex = p.rcx + t11 * lx + t12 * ly;
        float ey = p.rcy + t21 * lx + t22 * ly;
        // Matrix follows the body's local->screen transform, then flattens
        // vertically in screen space (blink)
        float a = t11 * ep[i].a + t12 * ep[i].c;
        float b = t11 * ep[i].b + t12 * ep[i].d;
        float c = (t21 * ep[i].a + t22 * ep[i].c) * lidK;
        float d = (t21 * ep[i].b + t22 * ep[i].d) * lidK;
        float na = alpha * (1.f - eyeX);
        if (na > 0.01f) {
            fb_fill_capsule(p.fb, ex, ey, a, b, c, d,
                            eyeW * p.scale / 2, eyeH * p.scale / 2, color565, na);
        }
        // Dizzy crossfade: X = two thinner capsules, rotated ±40° in the eye's
        // tangent plane before the body transform
        float xa = alpha * eyeX;
        if (xa > 0.01f) {
            for (int q = -1; q <= 1; q += 2) {
                float cq = cosf(q * 0.7f), sq = sinf(q * 0.7f);
                float ra = cq * ep[i].a - sq * ep[i].c;
                float rb = cq * ep[i].b - sq * ep[i].d;
                float rc = sq * ep[i].a + cq * ep[i].c;
                float rd = sq * ep[i].b + cq * ep[i].d;
                float a2 = t11 * ra + t12 * rc;
                float b2 = t11 * rb + t12 * rd;
                float c2 = (t21 * ra + t22 * rc) * lidK;
                float d2 = (t21 * rb + t22 * rd) * lidK;
                fb_fill_capsule(p.fb, ex, ey, a2, b2, c2, d2,
                                eyeW * p.scale * 0.28f, eyeH * p.scale * 0.52f,
                                color565, xa);
            }
        }
    }
}

// Axis-aligned segment as a capsule (u axis along the segment, v across)
static void seg565(Fb &fb, float x0, float y0, float x1, float y1, float hw,
                   uint16_t color, float alpha)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = hypotf(dx, dy);
    if (len < 1.f) return;
    float ux = dx / len, uy = dy / len;
    fb_fill_capsule(fb, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, ux, -uy, uy, ux,
                    len * 0.5f, hw, color, alpha);
}

void skin_draw_zzz(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    if (pose.zzzA < 0.01f) return;
    float s = p.scale * pose.bodyR;
    for (int i = 0; i < 3; i++) {
        // Each Z rises over 2.2 s, staggered by a third of the cycle
        float phase = fmodf(p.localT / 2.2f + i * (1.f / 3.f), 1.f);
        float a = pose.zzzA * sinf(phase * 3.14159265f);
        if (a < 0.02f) continue;
        float size = (0.16f + 0.14f * phase) * s;
        float zx = p.rcx + (0.58f + 0.10f * phase) * s;
        float zy = p.rcy - (0.80f + 0.50f * phase) * s;
        float hw = size * 0.62f, hh = size * 0.5f;
        float lw = size * 0.10f + 1.2f;  // stroke half-width (px)
        float x0 = zx - hw, x1 = zx + hw, y0 = zy - hh, y1 = zy + hh;
        seg565(p.fb, x0, y0, x1, y0, lw, p.ink, a);  // top bar
        seg565(p.fb, x1, y0, x0, y1, lw, p.ink, a);  // diagonal
        seg565(p.fb, x0, y1, x1, y1, lw, p.ink, a);  // bottom bar
    }
}

/* ---------------- grok-bot skin ---------------- */

static void grokbot_draw(CharacterDrawParams &p)
{
    skin_draw_rings_back(p);
    fb_draw_ellipse_over(p.fb, p.rcx, p.rcy, p.erot, p.erx, p.ery, p.ink, p.paper);
    skin_draw_dots(p);
    skin_draw_badge(p);
    skin_draw_eyes(p, p.m11, p.m12, p.m12, p.m22, p.pose->split, p.pose->eyeW,
                   p.pose->eyeH, p.paper);
    skin_draw_zzz(p);
    skin_draw_rings_front(p);
}

/* ---------------- registry ---------------- */

struct CharacterDef {
    const char *name;
    void (*draw)(CharacterDrawParams &);
};

static const CharacterDef CHARACTERS[] = {
    { "grokbot", grokbot_draw },
    { "pika",    pika_draw },
    { "rowlet",  rowlet_draw },
};
static int s_charIdx = 0;

int character_count() { return (int)(sizeof(CHARACTERS) / sizeof(CHARACTERS[0])); }
int character_index() { return s_charIdx; }
const char *character_name(int i) { return CHARACTERS[i].name; }

void character_select(int i)
{
    int n = character_count();
    s_charIdx = ((i % n) + n) % n;
}

void character_draw(CharacterDrawParams &p)
{
    CHARACTERS[s_charIdx].draw(p);
}

PixelRect CharacterRenderer::render(CharacterDrawParams params)
{
    params.fb.writes = nullptr;
    PixelRect dirty = initialized_ ? previous_ : PixelRect{0, 0, params.fb.w - 1, params.fb.h - 1};
    if (!dirty.empty()) fb_clear_rect(params.fb, dirty.x1, dirty.y1, dirty.x2, dirty.y2, params.paper);
    PixelRect current;
    params.fb.writes = &current;
    character_draw(params);
    dirty.include(current);
    previous_ = current;
    initialized_ = true;
    return dirty;
}
