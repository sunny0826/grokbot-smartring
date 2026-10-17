#include "home_controls.h"

namespace {
bool visible;
bool tracking, moved;
lv_point_t origin;
uint32_t started;

void touch(lv_event_t *event)
{
    auto *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        origin = point;
        started = lv_tick_get();
        tracking = true;
        moved = false;
    } else if (tracking && (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)) {
        const int dx = point.x - origin.x, dy = point.y - origin.y;
        moved = moved || dx * dx + dy * dy > 40 * 40;
        if (code == LV_EVENT_RELEASED) {
            if (!moved && lv_tick_elaps(started) < 400) {
                visible = true;
                lv_display_trigger_activity(lv_obj_get_display(lv_event_get_target_obj(event)));
            }
            tracking = false;
        }
    } else if (code == LV_EVENT_PRESS_LOST) tracking = false;
}
}

void home_controls_attach(lv_obj_t *surface)
{
    lv_obj_add_event_cb(surface, touch, LV_EVENT_ALL, nullptr);
}

bool home_controls_visible()
{
    if (visible && lv_display_get_inactive_time(nullptr) >= 10000) visible = false;
    return visible;
}
