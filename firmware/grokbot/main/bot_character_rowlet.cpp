// "Rowlet" skin (reference: reference/character-ref-2.png): a round tan owl —
// white two-lobed face mask with a center leaf, dark capsule eyes sitting high
// on the mask, a small orange beak, a green two-leaf bow tie on the chest, a
// light belly and tiny orange feet peeking out at the bottom. Flat matte style,
// same paper/ink look as grok-bot — no gradients, no gloss.
//
// Like pika, the head stays world-up: face features are composed with
// T = M_squash · R(upright). The beak rides the gaze sphere (same trick as
// pika's cheeks) so it keeps its distance to the pupils. Features shrink with
// bodyR (thinking state) and fade with eyeAlpha.
#include "bot_character.h"
#include <cmath>

static const uint16_t ROWLET_TAN    = rgb565(0xE5, 0xC6, 0x9C);
static const uint16_t ROWLET_MASK   = rgb565(0xFA, 0xF8, 0xF2);
static const uint16_t ROWLET_BELLY  = rgb565(0xE4, 0xE2, 0xDA);
static const uint16_t ROWLET_DARK   = rgb565(0x2B, 0x26, 0x22);
static const uint16_t ROWLET_ORANGE = rgb565(0xE8, 0x86, 0x30);
static const uint16_t ROWLET_GREEN  = rgb565(0x35, 0xA8, 0x8A);

// body-local -> screen 2x2: squash composed with the upright rotation
struct Mat2 {
    float t11, t12, t21, t22;
};

static inline Mat2 upright_mat(const CharacterDrawParams &p)
{
    float c = p.upCos, s = p.upSin;
    // T = M · R, R = [c -s; s c] (maps local up (0,-1) to world-up)
    return {
        p.m11 * c + p.m12 * s,  -p.m11 * s + p.m12 * c,
        p.m12 * c + p.m22 * s,  -p.m12 * s + p.m22 * c,
    };
}

static inline void local_xy(const CharacterDrawParams &p, const Mat2 &t,
                            float lx, float ly, float s, float &x, float &y)
{
    x = p.rcx + (t.t11 * lx + t.t12 * ly) * s;
    y = p.rcy + (t.t21 * lx + t.t22 * ly) * s;
}

// Ellipse fixed to the head-upright frame (squash-aware via the T columns)
static void local_ellipse(CharacterDrawParams &p, const Mat2 &t, float lx, float ly,
                          float hw, float hh, float s, uint16_t color, float a)
{
    float x, y;
    local_xy(p, t, lx, ly, s, x, y);
    fb_fill_capsule(p.fb, x, y, t.t11, t.t12, t.t21, t.t22, hw * s, hh * s, color, a);
}

static void local_tri(CharacterDrawParams &p, const Mat2 &t, float s,
                      float x0, float y0, float x1, float y1, float x2, float y2,
                      uint16_t color, float a)
{
    float xs[3], ys[3];
    local_xy(p, t, x0, y0, s, xs[0], ys[0]);
    local_xy(p, t, x1, y1, s, xs[1], ys[1]);
    local_xy(p, t, x2, y2, s, xs[2], ys[2]);
    fb_fill_poly(p.fb, xs, ys, 3, color, a);
}

// Small orange feet peeking out below the belly (drawn before the belly, so
// the belly tucks them in; tips stick out past the body silhouette)
static void draw_feet(CharacterDrawParams &p, const Mat2 &t, float s, float a)
{
    for (int side = -1; side <= 1; side += 2) {
        local_tri(p, t, s,
                  side * 0.10f, 0.74f,
                  side * 0.42f, 0.82f,
                  side * 0.26f, 1.16f,
                  ROWLET_ORANGE, a);
    }
}

// Ellipse anchored on the gaze sphere (like pika's cheeks): az/el are degrees
// relative to the face forward, the tangent basis gives edge foreshortening,
// and the feature fades out when it swings around to the back.
static void sphere_ellipse(CharacterDrawParams &p, const Mat2 &t, float azDeg, float elDeg,
                           float hw, float hh, float s, uint16_t color, float alpha)
{
    float cx0, cy0, m[4], depth;
    sphereFrame(p.gaze, azDeg, elDeg, 1.f, cx0, cy0, m, depth);
    if (depth < 0.05f) return;
    float a = alpha * clampf((depth - 0.05f) / 0.25f);
    float x = p.rcx + (t.t11 * cx0 + t.t12 * cy0) * s;
    float y = p.rcy + (t.t21 * cx0 + t.t22 * cy0) * s;
    float A = t.t11 * m[0] + t.t12 * m[2];
    float B = t.t11 * m[1] + t.t12 * m[3];
    float C = t.t21 * m[0] + t.t22 * m[2];
    float D = t.t21 * m[1] + t.t22 * m[3];
    fb_fill_capsule(p.fb, x, y, A, B, C, D, hw * s, hh * s, color, a);
}

// White face mask riding the gaze sphere with the eyes: two big overlapping
// circles (the butterfly lobes, centered on the eyes) plus a vertical center
// leaf extending down between them
static void draw_mask(CharacterDrawParams &p, const Mat2 &t, float s, float a)
{
    for (int side = -1; side <= 1; side += 2)
        sphere_ellipse(p, t, side * 21.f, -4.f, 0.44f, 0.44f, s, ROWLET_MASK, a);
    sphere_ellipse(p, t, 0.f, 10.f, 0.15f, 0.32f, s, ROWLET_MASK, a);
}

// Beak: a small downward triangle just below the eye line, centered between
// the pupils (offsets in the sphere tangent plane)
static void draw_beak(CharacterDrawParams &p, const Mat2 &t, float s, float alpha)
{
    float cx0, cy0, m[4], depth;
    sphereFrame(p.gaze, 0.f, 10.f, 1.f, cx0, cy0, m, depth);
    if (depth < 0.05f) return;
    float a = alpha * clampf((depth - 0.05f) / 0.25f);
    const float dx[3] = { -0.11f, 0.11f, 0.f };
    const float dy[3] = { -0.03f, -0.03f, 0.16f };
    float xs[3], ys[3];
    for (int i = 0; i < 3; i++) {
        float px = cx0 + m[0] * dx[i] + m[1] * dy[i];
        float py = cy0 + m[2] * dx[i] + m[3] * dy[i];
        xs[i] = p.rcx + (t.t11 * px + t.t12 * py) * s;
        ys[i] = p.rcy + (t.t21 * px + t.t22 * py) * s;
    }
    fb_fill_poly(p.fb, xs, ys, 3, ROWLET_ORANGE, a);
}

// Green two-leaf bow tie on the chest, leaves angled outward-down
static void draw_bowtie(CharacterDrawParams &p, const Mat2 &t, float s, float a)
{
    const float ca = 0.940f, sa = 0.342f;  // cos/sin 20°
    for (int side = -1; side <= 1; side += 2) {
        float x, y;
        local_xy(p, t, side * 0.25f, 0.50f, s, x, y);
        // leaf long axis ul = side·(ca, sa), across vl = (-sa, side·ca)
        float au = t.t11 * side * ca + t.t12 * sa;
        float cu = t.t21 * side * ca + t.t22 * sa;
        float av = -t.t11 * sa + t.t12 * side * ca;
        float cv = -t.t21 * sa + t.t22 * side * ca;
        fb_fill_capsule(p.fb, x, y, au, av, cu, cv, 0.27f * s, 0.115f * s,
                        ROWLET_GREEN, a);
        // pointed leaf tip: small triangle past the capsule's round end
        float du[3] = { 0.20f, 0.20f, 0.40f };
        float dv[3] = { 0.085f, -0.085f, 0.f };
        float xs[3], ys[3];
        for (int i = 0; i < 3; i++) {
            xs[i] = x + (au * du[i] + av * dv[i]) * s;
            ys[i] = y + (cu * du[i] + cv * dv[i]) * s;
        }
        fb_fill_poly(p.fb, xs, ys, 3, ROWLET_GREEN, a);
    }
    local_ellipse(p, t, 0.f, 0.50f, 0.065f, 0.065f, s, ROWLET_GREEN, a);
}

void rowlet_draw(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    float s = p.scale * pose.bodyR;   // features shrink with the body (thinking state)
    float faceA = pose.eyeAlpha;      // face fades out together with the eyes
    float headA = clampf((pose.bodyR - 0.3f) / 0.3f);
    Mat2 t = upright_mat(p);

    skin_draw_rings_back(p);

    // Flat body (writes the whole bbox)
    fb_draw_ellipse_over(p.fb, p.rcx, p.rcy, p.erot, p.erx, p.ery, ROWLET_TAN, p.paper);

    if (headA > 0.01f) draw_feet(p, t, s, headA);
    if (faceA > 0.01f && headA > 0.01f) {
        float a = faceA * headA;
        local_ellipse(p, t, 0.f, 0.70f, 0.58f, 0.30f, s, ROWLET_BELLY, a);
        draw_mask(p, t, s, a);
        draw_beak(p, t, s, a);
        draw_bowtie(p, t, s, a);
    }

    skin_draw_dots(p);
    skin_draw_badge(p);

    // Big dark capsule eyes high on the mask (el < 0 = above the eye line);
    // pupils gaze downhill, same blink/dizzy treatment as the other skins
    skin_draw_eyes(p, t.t11, t.t12, t.t21, t.t22, 21.f, 0.28f, 0.46f, ROWLET_DARK, -4.f);

    skin_draw_zzz(p);
    skin_draw_rings_front(p);
}
