#pragma once

#include "lvgl.h"

// LVGL task only. Attach to the home background and character canvas.
void home_controls_attach(lv_obj_t *surface);
bool home_controls_visible();
