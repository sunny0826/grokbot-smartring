// Desktop preview harness for character skins: renders representative poses of
// each skin to PPM images, so geometry can be tuned without flashing the device.
// Mirrors the parameter math of main.cpp frame_cb (squash matrix, upright
// rotation, pose evaluation). Convert: python3 sim/ppm2png.py
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "bot_character.h"

static const int CANVAS = 320;
static const float SCALE = 100.f;

static void savePPM(const char *path, Fb &fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
    for (int i = 0; i < fb.w * fb.h; i++) {
        uint16_t px = fb.px[i];
        fputc(((px >> 11) & 31) * 255 / 31, f);
        fputc(((px >> 5) & 63) * 255 / 63, f);
        fputc((px & 31) * 255 / 31, f);
    }
    fclose(f);
}

struct Scene {
    const char *name;
    int skin;        // CHARACTERS[] index
    BotState state;  // carousel pose
    HeadGaze gaze;   // resolved gaze (gravity mode values)
    float ux, uy;    // world-up in screen coords
    float squash, squashDir;
};

int main()
{
    uint16_t *buf = (uint16_t *)malloc(CANVAS * CANVAS * 2);
    Fb fb = { buf, CANVAS, CANVAS };
    const uint16_t PAPER = rgb565(0xec, 0xe7, 0xdb);
    const uint16_t INK = rgb565(0x0a, 0x0a, 0x0c);

    const Scene scenes[] = {
        // device flat: resting gaze, upright frame
        { "pika_flat",    1, BotState::Idle,     REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        // tilted 90° right: downhill = +x, gaze looks right, world-up = -x
        { "pika_right",   1, BotState::Idle,     { 38.f, 0.f, -18.f },    -1.f, 0.f,     0.15f, 0.f },
        // tilted 45° down-right
        { "pika_down45",  1, BotState::Idle,     { 26.9f, -26.9f, -12.7f }, -0.7071f, -0.7071f, 0.1f, 0.7854f },
        // upside down: downhill = -y
        { "pika_upsided", 1, BotState::Idle,     { 0.f, 38.f, 0.f },       0.f, 1.f,     0.f,   0.f },
        // thinking state (carousel)
        { "pika_think",   1, BotState::Thinking, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        // grokbot references
        { "grok_flat",    0, BotState::Idle,     REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        { "grok_right",   0, BotState::Idle,     { 38.f, 0.f, -18.f },    -1.f, 0.f,     0.15f, 0.f },
    };

    for (const Scene &sc : scenes) {
        character_select(sc.skin);
        BotPose pose;
        poseFor(sc.state, 1.0f, pose);

        // frame_cb's squash ellipse + matrix
        float sq = 1.f - 0.22f * sc.squash, st = 1.f + 0.16f * sc.squash;
        float rcx = CANVAS / 2.f, rcy = CANVAS / 2.f;
        float erx = st * SCALE * pose.bodyR, ery = sq * SCALE * pose.bodyR;
        float erot = sc.squashDir + 3.14159265f / 2.f;
        float cr = cosf(sc.squashDir), sr = sinf(sc.squashDir);
        float m11 = st * cr * cr + sq * sr * sr;
        float m12 = cr * sr * (st - sq);
        float m22 = st * sr * sr + sq * cr * cr;

        float um = hypotf(sc.ux, sc.uy);
        float upSin = sc.ux / um, upCos = -sc.uy / um;

        CharacterDrawParams cp = {
            fb, rcx, rcy, erot, erx, ery, m11, m12, m22,
            sc.gaze, &pose, 1.f /* lid open */, SCALE, 1.0f /* localT */,
            INK, PAPER, upCos, upSin,
        };
        fb_clear(fb, PAPER);
        character_draw(cp);
        char path[256];
        snprintf(path, sizeof(path), "out/%s.ppm", sc.name);
        savePPM(path, fb);
        printf("wrote %s\n", path);
    }
    return 0;
}
