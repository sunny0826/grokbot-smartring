#pragma once

struct BodyMotion {
    float x = 0, y = 0, vx = 0, vy = 0;
    float squash = 0, squash_direction = 0;
    void advance(float elapsed, float gx, float gy, bool centered);
};
