#include "voice_ui.h"
#include "voice_timing.h"
#include "voice_page.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

static VoiceSnapshot state;
static int64_t now = 1000000;
static bool covered;
static int failure = -1, strip;
static bool frame_failed, stale;
static unsigned completed;
static std::vector<uint16_t> pixels(360 * 360);
extern "C" int64_t voice_timing_now_us() { return now; }
extern "C" __attribute__((weak)) void my_bot_display_flush_completed(
    void *, int, int, int, int, bool, bool, bool, int64_t) {}
VoiceSnapshot voice_snapshot() { return state; }
bool voice_action(VoiceAction, uint32_t) { return true; }
uint32_t voice_hold_begin() { return 1; }
void voice_hold_end(uint32_t, VoiceHoldEnd) {}
bool voice_copy_page(uint32_t generation, uint8_t *out, size_t size) {
    if (generation != state.generation || size != VOICE_PAGE_BYTES) return false;
    for (size_t i = 0; i < size; i += 2) { out[i] = 0x1f; out[i + 1] = 0xf8; }
    return true;
}
static VoiceTimingRecord record() {
    VoiceTimingRecord rows[VOICE_TIMING_CAPACITY];
    const auto count = voice_timing_snapshot(rows, VOICE_TIMING_CAPACITY);
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(rows[i].request_id, state.request_id)) return rows[i];
    assert(false); return {};
}
static int64_t endpoint() { return record().at_us[size_t(VoiceTimingStage::FirstVisible)]; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data) {
    const bool last = lv_display_flush_is_last(display);
    const bool success = !(failure == strip || (failure == 99 && last));
    frame_failed |= !success;
    const size_t stride = lv_display_get_buf_active(display)->header.stride;
    if (success) for (int y = area->y1; y <= area->y2; ++y) for (int x = area->x1; x <= area->x2; ++x) {
        const auto *source = data + (y - area->y1) * stride + (x - area->x1) * 2;
        pixels[y * 360 + x] = source[0] | uint16_t(source[1]) << 8;
    }
    if (stale && strip == 1) { ++state.generation; stale = false; }
    const auto before = endpoint();
    now += 50; // Controlled DMA completion boundary, not real hardware time.
    my_bot_display_flush_completed(display, area->x1, area->y1, area->x2, area->y2,
                                    strip == 0, last, success, now);
    if (!last || frame_failed) assert(endpoint() == before);
    if (!before && endpoint()) {
        assert(last && !frame_failed && endpoint() == now);
        for (int y = 96; y < 264; ++y) for (int x = 44; x < 316; ++x)
            assert(pixels[y * 360 + x] == 0xf81f);
        ++completed;
    }
    ++strip;
    lv_display_flush_ready(display);
}
static void refresh_event(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_REFR_START) { strip = 0; frame_failed = false; }
    else if (frame_failed) lv_obj_invalidate(lv_screen_active());
}
static void advance(unsigned ms, bool update = true) {
    now += ms * 1000;
    lv_tick_inc(ms);
    if (update) voice_ui_update(lv_tick_get(), covered);
    lv_timer_handler();
    lv_refr_now(nullptr);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    std::strcpy(state.request_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    state.phase = VoicePhase::ViewingReply; state.generation = 1; state.page_count = 1;
    voice_timing_start(state.request_id, scenario == "boot" ? 0 : now - 500000);
    lv_init(); auto *display = lv_display_create(360, 360);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    static uint8_t buffer[360 * 40 * 2];
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, refresh_event, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(display, refresh_event, LV_EVENT_REFR_READY, nullptr);
    voice_ui_create(lv_screen_active());
    covered = scenario == "covered";
    if (scenario == "fail-first") failure = 0;
    if (scenario == "fail-middle") failure = 2;
    if (scenario == "fail-last") failure = 99;
    lv_obj_t *overlay = nullptr;
    if (scenario == "overlay") {
        overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(overlay, 100, 100); lv_obj_center(overlay);
    }
    advance(1); assert(!endpoint());
    advance(80); assert(!endpoint()); // Existing 160 ms fade must still be in progress.
    if (scenario == "stale") stale = true;
    advance(100);
    if (scenario == "normal") assert(endpoint() && completed == 1);
    else {
        assert(!endpoint());
        if (scenario == "boot") {
            advance(200); assert(!endpoint());
            std::puts("PASS real LVGL: boot has no fabricated release"); return 0;
        }
        if (overlay) lv_obj_delete(overlay);
        covered = false; failure = -1;
        advance(1);
        advance(200);
        assert(endpoint() && completed == 1);
        if (scenario == "stale") assert(record().generation == 2);
    }
    const auto first = endpoint();
    advance(300); assert(endpoint() == first && completed == 1);
    lv_display_delete(display);
    std::printf("PASS real LVGL: %s, complete opaque page, once at final successful flush\n", argv[1]);
}
