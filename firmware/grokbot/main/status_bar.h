#pragma once

#include "lvgl.h"

// Create once, after the character canvas, while holding the LVGL lock.
void status_bar_create(lv_obj_t *screen, bool battery_available);
