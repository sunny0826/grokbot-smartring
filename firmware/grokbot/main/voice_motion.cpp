#include "voice_motion.h"
#include <cmath>

namespace {
lv_obj_t *meter, *recording;
VoicePhase previous = VoicePhase::Idle;
uint32_t entered, updated;
float envelope;

void opacity(void *object, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), value, 0);
}
}

void voice_motion_reset()
{
    meter = recording = nullptr;
    previous = VoicePhase::Idle;
    entered = updated = 0;
    envelope = 0;
}

void voice_motion_create(lv_obj_t *level, lv_obj_t *recording_dot)
{
    voice_motion_reset();
    meter = level;
    recording = recording_dot;
}

float voice_motion_update(VoicePhase phase, uint16_t peak, uint32_t now, bool visible)
{
    if (!meter) return 0;
    const float dt = fminf((now - updated) / 1000.f, 0.1f);
    updated = now;
    if (phase != previous) {
        previous = phase;
        entered = now;
        envelope = 0;
        if (visible) lv_bar_set_value(meter, 0, LV_ANIM_OFF);
    }
    const float seconds = (now - entered) / 1000.f;
    if (phase == VoicePhase::Recording) {
        const float target = fminf(peak, 32767);
        const float response = target > envelope ? 0.045f : 0.18f;
        envelope += (target - envelope) * (1.f - expf(-dt / response));
        if (visible) {
            lv_bar_set_value(meter, static_cast<int32_t>(envelope + 0.5f), LV_ANIM_OFF);
            const float pulse = 0.5f + 0.5f * cosf(seconds * 5.235988f);
            lv_obj_set_style_opa(recording, static_cast<lv_opa_t>(128 + 127 * pulse), 0);
        }
    }
    return envelope / 32767.f;
}

void voice_motion_reveal(lv_obj_t *object)
{
    lv_anim_delete(object, opacity);
    lv_obj_set_style_opa(object, LV_OPA_TRANSP, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_values(&animation, 0, LV_OPA_COVER);
    lv_anim_set_duration(&animation, 160);
    lv_anim_set_exec_cb(&animation, opacity);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void voice_motion_finish_reveal(lv_obj_t *object)
{
    lv_anim_delete(object, opacity);
    lv_obj_set_style_opa(object, LV_OPA_COVER, 0);
}
