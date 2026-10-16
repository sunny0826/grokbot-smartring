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
    float poseT;     // local time inside the pose
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
        { "pika_flat",    1, BotState::Idle,     1.0f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        // tilted 90° right: downhill = +x, gaze looks right, world-up = -x
        { "pika_right",   1, BotState::Idle,     1.0f, { 38.f, 0.f, -18.f },    -1.f, 0.f,     0.15f, 0.f },
        // upside down: downhill = -y
        { "pika_upsided", 1, BotState::Idle,     1.0f, { 0.f, 38.f, 0.f },       0.f, 1.f,     0.f,   0.f },
        { "pika_happy",   1, BotState::Happy,    0.45f, REST_GAZE,               0.f, -1.f,    0.f,   0.f },
        { "pika_sleepy",  1, BotState::Sleepy,   2.5f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        { "pika_dizzy",   1, BotState::Dizzy,    0.8f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        // grokbot references
        { "grok_flat",    0, BotState::Idle,     1.0f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        { "grok_happy",   0, BotState::Happy,    0.45f, REST_GAZE,               0.f, -1.f,    0.f,   0.f },
        { "grok_sleepy",  0, BotState::Sleepy,   2.5f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
        { "grok_dizzy",   0, BotState::Dizzy,    0.8f, REST_GAZE,                0.f, -1.f,    0.f,   0.f },
    };

    for (const Scene &sc : scenes) {
        character_select(sc.skin);
        BotPose pose;
        poseFor(sc.state, sc.poseT, pose);

        // frame_cb's squash ellipse + matrix (impact squash + pose squash)
        float squashAll = sc.squash + pose.squashAdd;
        float squashDir = sc.squash > 0.03f ? sc.squashDir : 1.5707963f;
        float sq = 1.f - 0.22f * squashAll, st = 1.f + 0.16f * squashAll;
        float rcx = CANVAS / 2.f + pose.bodyCx * SCALE;
        float rcy = CANVAS / 2.f + pose.bodyCy * SCALE;
        float erx = st * SCALE * pose.bodyR, ery = sq * SCALE * pose.bodyR;
        float erot = squashDir + 3.14159265f / 2.f;
        float cr = cosf(squashDir), sr = sinf(squashDir);
        float m11 = st * cr * cr + sq * sr * sr;
        float m12 = cr * sr * (st - sq);
        float m22 = st * sr * sr + sq * cr * cr;

        float um = hypotf(sc.ux, sc.uy);
        float upSin = sc.ux / um, upCos = -sc.uy / um;

        float lid = 1.f - pose.eyeClose;  // blink system not simulated
        // main.cpp: gravity gaze only in Idle; other states carry their own gaze
        HeadGaze gz = sc.state == BotState::Idle ? sc.gaze : pose.gaze;
        CharacterDrawParams cp = {
            fb, rcx, rcy, erot, erx, ery, m11, m12, m22,
            gz, &pose, lid, SCALE, sc.poseT,
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
