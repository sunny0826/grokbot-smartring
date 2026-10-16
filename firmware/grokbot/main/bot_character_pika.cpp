// "Pika" skin (reference: reference/character-ref.png): a flat yellow blob with
// capsule eyes (same shape as grok-bot's) and round red cheeks just below the
// eye line. No ears, no mouth. Flat matte style, same paper/ink look as
// grok-bot — no gradients, no gloss.
//
// The head stays world-up: everything is composed with T = M_squash ·
// R(upright), so the face always faces away from gravity while the body rolls.
// Cheeks ride the same gaze sphere as the pupils, so the eye-cheek distance is
// constant. Features shrink and fade with bodyR (thinking state) and the face
// fades with eyeAlpha.
#include "bot_character.h"
#include <cmath>

static const uint16_t PIKA_YELLOW = rgb565(0xFF, 0xCB, 0x2B);
static const uint16_t PIKA_BLACK  = rgb565(0x26, 0x26, 0x2A);
static const uint16_t PIKA_RED    = rgb565(0xE8, 0x40, 0x2A);

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

static void draw_cheeks(CharacterDrawParams &p, const Mat2 &t, float s, float alpha)
{
    // Anchored on the SAME gaze sphere as the eyes (az ±40°, 15° below the eye
    // line): the eye-cheek distance stays constant no matter where the pupils
    // look. Near the silhouette edge they fade out like the eyes. Drawn as
    // perfect circles: squashed cheek dots read as smears
    for (int side = -1; side <= 1; side += 2) {
        float cx0, cy0, m[4], depth;
        sphereFrame(p.gaze, side * 40.f, 15.f, 1.f, cx0, cy0, m, depth);
        if (depth < 0.05f) continue;  // swung around to the back
        float a = alpha * clampf((depth - 0.05f) / 0.25f);
        float sx = p.rcx + (t.t11 * cx0 + t.t12 * cy0) * s;
        float sy = p.rcy + (t.t21 * cx0 + t.t22 * cy0) * s;
        fb_fill_ellipse(p.fb, sx, sy, 0.f, 0.17f * s, 0.17f * s, PIKA_RED, a);
    }
}

void pika_draw(CharacterDrawParams &p)
{
    const BotPose &pose = *p.pose;
    float s = p.scale * pose.bodyR;   // features shrink with the body (thinking state)
    float faceA = pose.eyeAlpha;      // face fades out together with the eyes
    // Cheeks fade out as the ball shrinks into the thinking dot
    float headA = clampf((pose.bodyR - 0.3f) / 0.3f);
    Mat2 t = upright_mat(p);

    skin_draw_rings_back(p);

    // Flat body (writes the whole bbox)
    fb_draw_ellipse_over(p.fb, p.rcx, p.rcy, p.erot, p.erx, p.ery, PIKA_YELLOW, p.paper);

    skin_draw_dots(p);
    skin_draw_badge(p);

    if (faceA > 0.01f && headA > 0.01f) draw_cheeks(p, t, s, faceA * headA);
    // Same tall capsule eyes as grok-bot (EYE_SPLIT/EYE_W/EYE_H from bot_face.h);
    // positions ride the upright-rotated frame, pupils gaze downhill
    skin_draw_eyes(p, t.t11, t.t12, t.t21, t.t22, EYE_SPLIT, EYE_W, EYE_H, PIKA_BLACK);

    skin_draw_rings_front(p);
}
