#include "bot_motion.h"
#include "bot_math.h"
#include <cmath>

void BodyMotion::advance(float elapsed, float gx, float gy, bool centered)
{
    // Bound catch-up after a stall; normal frame time is integrated in full.
    const float duration = fminf(fmaxf(elapsed, 0.f), .25f);
    const int steps = static_cast<int>(ceilf(duration / .025f));
    if (!steps) return;
    const float dt = duration / steps;
    for (int i = 0; i < steps; ++i) {
        if (centered) {
            const float k = 1.f - expf(-dt / .3f);
            x -= x * k; y -= y * k;
            vx = vy = 0;
        } else {
            vx += gx * 2.4f * dt; vy += gy * 2.4f * dt;
        }
        const float damping = expf(-.9f * dt);
        vx *= damping; vy *= damping;
        x += vx * dt; y += vy * dt;
        const float radius = hypotf(x, y);
        if (radius > .52f) {
            const float nx = x / radius, ny = y / radius;
            x = nx * .52f; y = ny * .52f;
            const float speed = vx * nx + vy * ny;
            if (speed > 0) {
                vx -= 1.42f * speed * nx;
                vy -= 1.42f * speed * ny;
                const float impact = clampf(speed * .3f);
                if (impact > squash) { squash = impact; squash_direction = atan2f(ny, nx); }
            }
        }
        const float speed = hypotf(vx, vy);
        if (speed > .8f) {
            const float motion = clampf((speed - .8f) * .08f, 0.f, .5f);
            if (motion > squash) { squash = motion; squash_direction = atan2f(vy, vx); }
        }
        squash *= expf(-5.f * dt);
    }
}
