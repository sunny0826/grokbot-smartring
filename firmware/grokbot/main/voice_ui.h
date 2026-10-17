#pragma once

#include "lvgl.h"

// Create and access from the LVGL task while its display lock is held.
void voice_ui_create(lv_obj_t *screen);
bool voice_ui_active();
