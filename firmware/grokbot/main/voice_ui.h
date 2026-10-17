#pragma once

#include "lvgl.h"
#include "voice_character.h"

// Create and access from the LVGL task while its display lock is held.
void voice_ui_create(lv_obj_t *screen);
bool voice_ui_active();
// Commit labels and the matching character frame before yielding to LVGL refresh.
VoicePresentation voice_ui_update(uint32_t now, bool covered);
