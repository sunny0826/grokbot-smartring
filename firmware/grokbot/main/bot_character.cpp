// Character skins: registry + the default grok-bot skin (ported from bloub,
// jeremy-prt/bloub, MIT). Draw order back rings -> body -> dots -> badge ->
// eyes -> front rings yields real depth ordering for the orbit.
#include "bot_character.h"
#include <cmath>

// Skins living in their own translation units:
void pika_draw(CharacterDrawParams &p);  // bot_character_pika.cpp

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
                    float splitDeg, float eyeW, float eyeH, uint16_t color565)
{
    const BotPose &pose = *p.pose;
    if (pose.eyeAlpha <= 0.01f) return;
    EyePose ep[2];
    eyePoses(p.gaze, p.scale, splitDeg, ep);
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
        fb_fill_capsule(p.fb, ex, ey, a, b, c, d,
                        eyeW * p.scale / 2, eyeH * p.scale / 2, color565, alpha);
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
    skin_draw_rings_front(p);
}

/* ---------------- registry ---------------- */

struct CharacterDef {
    const char *name;
    void (*draw)(CharacterDrawParams &);
    bool fullClear;  // draws outside the body bbox -> no dirty-rect fast path
};

static const CharacterDef CHARACTERS[] = {
    { "grokbot", grokbot_draw, false },
    { "pika",    pika_draw,    true  },  // ears extend past the body bbox
};
static int s_charIdx = 0;

int character_count() { return (int)(sizeof(CHARACTERS) / sizeof(CHARACTERS[0])); }
int character_index() { return s_charIdx; }
const char *character_name(int i) { return CHARACTERS[i].name; }
bool character_full_clear() { return CHARACTERS[s_charIdx].fullClear; }

void character_select(int i)
{
    int n = character_count();
    s_charIdx = ((i % n) + n) % n;
}

void character_draw(CharacterDrawParams &p)
{
    CHARACTERS[s_charIdx].draw(p);
}
