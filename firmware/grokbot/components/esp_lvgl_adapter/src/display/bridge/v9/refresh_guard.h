#pragma once
#include "lvgl.h"
#include <stdbool.h>

typedef struct {
    lv_display_t *display;
    bool failed;
} display_refresh_guard_t;

static void display_refresh_guard_event(lv_event_t *event)
{
    display_refresh_guard_t *guard = (display_refresh_guard_t *)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_REFR_START) {
        guard->failed = false;
    } else if (guard->failed) {
        /* Refresh has cleared its invalidation list; retry all composited layers next time. */
        lv_obj_invalidate(lv_display_get_layer_sys(guard->display));
    }
}

static inline void display_refresh_guard_detach(display_refresh_guard_t *guard)
{
    if (guard->display)
        lv_display_remove_event_cb_with_user_data(guard->display, display_refresh_guard_event, guard);
    guard->display = NULL;
}

static inline void display_refresh_guard_attach(display_refresh_guard_t *guard, lv_display_t *display)
{
    display_refresh_guard_detach(guard);
    guard->display = display;
    guard->failed = false;
    if (!display) return;
    lv_display_add_event_cb(display, display_refresh_guard_event, LV_EVENT_REFR_START, guard);
    lv_display_add_event_cb(display, display_refresh_guard_event, LV_EVENT_REFR_READY, guard);
}
