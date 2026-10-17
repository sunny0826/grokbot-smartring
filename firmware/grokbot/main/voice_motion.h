#pragma once
#include "lvgl.h"
#include "voice.h"

void voice_motion_create(lv_obj_t *level, lv_obj_t *recording_dot);
float voice_motion_update(VoicePhase phase, uint16_t peak, uint32_t now, bool visible);
void voice_motion_reset();
void voice_motion_reveal(lv_obj_t *object);
void voice_motion_finish_reveal(lv_obj_t *object);
