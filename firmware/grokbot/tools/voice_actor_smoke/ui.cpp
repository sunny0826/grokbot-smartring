#include "fixture.hpp"
#include "voice_ui.h"
#include "home_controls.h"
#include <cstdio>
namespace {
std::vector<uint8_t> pixels(360 * 360 * 3);
void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data) {
    const size_t stride = lv_display_get_buf_active(display)->header.stride;
    for (int y = area->y1; y <= area->y2; ++y) for (int x = area->x1; x <= area->x2; ++x) {
        const auto *source = data + (y - area->y1) * stride + (x - area->x1) * 2;
        const uint16_t color = source[0] | uint16_t(source[1]) << 8;
        const auto offset = (y * 360 + x) * 3;
        pixels[offset] = ((color >> 11) & 31) * 255 / 31;
        pixels[offset + 1] = ((color >> 5) & 63) * 255 / 63;
        pixels[offset + 2] = (color & 31) * 255 / 31;
    }
    lv_display_flush_ready(display);
}
bool visible(lv_obj_t *object) {
    for (auto *parent = object; parent; parent = lv_obj_get_parent(parent))
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
lv_obj_t *find(lv_obj_t *object, const char *label) {
    if (lv_obj_check_type(object, &lv_label_class) && std::strcmp(lv_label_get_text(object), label) == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto *found = find(lv_obj_get_child(object, i), label)) return found;
    return nullptr;
}
lv_obj_t *find_image(lv_obj_t *object) {
    if (lv_obj_check_type(object, &lv_image_class)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto *found = find_image(lv_obj_get_child(object, i))) return found;
    return nullptr;
}
void tick() {
    lv_tick_inc(50); voice_ui_update(lv_tick_get(), false); lv_timer_handler(); lv_refr_now(nullptr);
}
void capture(const char *name) {
    tick(); FILE *file = std::fopen(name, "wb"); fixture::require(file, "cannot create UI capture");
    std::fprintf(file, "P6\n360 360\n255\n");
    std::fwrite(pixels.data(), 1, pixels.size(), file); std::fclose(file);
}
void click_read() {
    auto *label = find(lv_screen_active(), "已读"); fixture::require(label && visible(label), "read button not visible");
    auto *button = lv_obj_get_parent(label);
    lv_obj_send_event(button, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(button, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    lv_obj_remove_state(button, LV_STATE_PRESSED);
}
}
namespace fixture {
void run_ui_scenario(bool fail_save) {
    seed_pending(); voice_start(); open_reply();
    lv_init(); auto *display = lv_display_create(360, 360);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    static uint8_t buffer[360 * 40 * 2];
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0xece7db), 0);
    voice_ui_create(lv_screen_active()); home_controls_attach(lv_screen_active());
    tick(); require(voice_ui_active(), "real reading UI not active");
    auto *page = find_image(lv_screen_active()); require(page && visible(page), "reply page not visible");
    capture(fail_save ? "ui-before-failure.ppm" : "ui-before-read.ppm");
    if (fail_save) host::fail_commit = true;
    click_read();
    if (fail_save) {
        require(eventually([] { return voice_snapshot().error[0]; }), "UI click did not attempt local save");
        tick();
        auto *title = find(lv_screen_active(), "保存失败");
        require(title && visible(title), "save failure title invisible");
        require(voice_ui_active() && visible(page), "save failure hid original page");
        require(voice_snapshot().phase == VoicePhase::ViewingReply && reads == 0, "save failure left reading or started network");
        capture("ui-save-failure.ppm");
        host::fail_commit = false; click_read();
    }
    require(eventually([] { return voice_snapshot().phase == VoicePhase::Idle; }, 250), "LVGL click did not return home");
    require(eventually([] { return reads == 1; }), "UI did not reach blocked Read");
    tick(); require(!voice_ui_active() && !visible(page), "reading overlay remained visible at home");
    require(read_permits == 0, "Read was released before UI returned home");
    capture(fail_save ? "ui-save-retry-home.ppm" : "ui-read-home.ppm");
    const auto hold = record(); cancel(hold);
}
}
