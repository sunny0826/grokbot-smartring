#pragma once
#include "bot_character.h"
#include <cmath>

inline CharacterDrawParams render_case(Fb fb, const BotPose &pose, float time, float offset = 0)
{
    const float angle = 0.7f * sinf(time * 0.9f);
    const float squash = 0.25f * (1 + sinf(time * 2.3f)) + pose.squashAdd;
    const float st = (1 + 0.16f * squash) * (1 + 0.015f * sinf(time * 1.8f));
    const float sq = 1 - 0.22f * squash;
    const float c = cosf(angle), s = sinf(angle);
    return {fb, 160 + offset + 40 * sinf(time * 0.7f) + pose.bodyCx * 100,
        160 + 35 * cosf(time * 0.6f) + pose.bodyCy * 100,
        angle + 1.5707963f, st * 100 * pose.bodyR, sq * 100 * pose.bodyR,
        st*c*c + sq*s*s, c*s*(st-sq), st*s*s + sq*c*c,
        {38*sinf(time), 30*cosf(time * 0.9f), 18*sinf(time * 0.5f)}, &pose,
        fminf(0.5f + 0.5f*cosf(time * 2), 1-pose.eyeClose), 100, time,
        rgb565(10,10,12), rgb565(236,231,219), cosf(time), sinf(time)};
}
