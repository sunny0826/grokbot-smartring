// Character skin interface — everything a character looks like lives behind this file.
//
// The physics/carousel core (main.cpp) resolves everything down to the plain
// numbers in CharacterDrawParams; a skin's single job is to paint the character
// into the framebuffer for one frame.
//
// Multiple skins coexist at runtime (see the registry at the bottom of
// bot_character.cpp); the user switches with a left/right swipe on the screen.
// To add a new character:
//   1. create bot_character_<name>.cpp implementing `void <name>_draw(CharacterDrawParams &)`
//      (compose the shared skin_* helpers and/or the fb_* primitives),
//   2. register it in the CHARACTERS[] table in bot_character.cpp,
//   3. add the .cpp to main/CMakeLists.txt.
// Physics (roll/bounce/jelly), gaze, blink schedule and the idle carousel all
// keep working unchanged, because they live on the producing side of this struct.
#pragma once
#include <cstdint>
#include "bot_render.h"
#include "bot_face.h"
#include "bot_states.h"

struct CharacterDrawParams {
    Fb fb;               // framebuffer (pointer + size, by value)
    float rcx, rcy;      // body center, screen px (gravity offset + pose offset applied)
    float erot, erx, ery;// body ellipse: rotation (rad) and semi-axes (px), squash included
    float m11, m12, m22; // squash 2x2 (symmetric, M = R·S·Rᵀ) — apply to facial features
    HeadGaze gaze;       // resolved gaze for this frame (after pose blending)
    const BotPose *pose; // carousel pose: body radius, eye size/alpha, dots/badge/rings
    float lid;           // eyelid openness 1=open, 0=closed (blink + poke + morph mask)
    float scale;         // px per ball radius (main.cpp SCALE)
    float localT;        // seconds inside the current carousel state (ring stagger)
    uint16_t ink, paper; // character/background colors (RGB565)
    /* Rotation aligning the character's local up (0,-1) with world-up, as cos/sin
     * (identity = 1/0). Computed from smoothed gravity; eases back to identity when
     * the device lies flat. Skins that keep their head upright compose it as
     * T = M·R(up); round blobs (grokbot) ignore it. */
    float upCos, upSin;
};

/* ---------------- runtime skin registry ---------------- */

int character_count();
int character_index();
const char *character_name(int i);
void character_select(int i);  // wraps around
// True if the current skin draws outside the body-ellipse bbox (e.g. ears) and
// therefore needs a full clear every frame instead of the dirty-rect fast path.
bool character_full_clear();

// Paint one frame of the current character. The implementation must write every
// pixel inside the body ellipse bbox unless character_full_clear() is true for it
// (main.cpp's dirty-rect fast path relies on it). Non-const: drawing mutates the fb.
void character_draw(CharacterDrawParams &p);

/* ---------------- shared building blocks for skins ----------------
 * Implemented in bot_character.cpp; usable from any skin file. All of them
 * honor the carousel pose (alphas fade decorations in/out with the morphs). */

// Orbit rings, back half (z<0, occluded) / front half (z>=0, over the body)
void skin_draw_rings_back(CharacterDrawParams &p);
void skin_draw_rings_front(CharacterDrawParams &p);
// Thinking state's two side dots, and the notify badge
void skin_draw_dots(CharacterDrawParams &p);
void skin_draw_badge(CharacterDrawParams &p);
// Spherical-projection eyes as capsules: gaze-following, blink-flattened.
// (t11 t12 / t21 t22) maps body-local units to screen px — pass the squash
// matrix M for a blob, or M·R(upright) for a head that stays world-up.
// splitDeg = half eye separation on the sphere (degrees);
// eyeW/eyeH = full eye size in ball-radius units.
void skin_draw_eyes(CharacterDrawParams &p, float t11, float t12, float t21, float t22,
                    float splitDeg, float eyeW, float eyeH, uint16_t color565);
// Sleepy state's Zzz bubbles: three ink Z's floating up off the head, animated
// by p.localT, opacity from pose.zzzA
void skin_draw_zzz(CharacterDrawParams &p);
