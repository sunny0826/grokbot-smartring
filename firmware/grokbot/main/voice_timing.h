#pragma once
#include <stddef.h>
#include <stdint.h>

enum class VoiceTimingStage : uint8_t {
    UploadStarted, Connected, UploadConfirmed, ReplyReady, PageValidated, FirstVisible, Count
};
constexpr size_t VOICE_TIMING_CAPACITY = 8;
struct VoiceTimingRecord {
    char request_id[33] = {};
    int64_t released_us = 0;
    int64_t at_us[static_cast<size_t>(VoiceTimingStage::Count)] = {};
    uint32_t generation = 0;
    uint16_t page_index = 0;
};
void voice_timing_start(const char *request_id, int64_t released_us);
void voice_timing_mark(const char *request_id, VoiceTimingStage stage);
size_t voice_timing_snapshot(VoiceTimingRecord *out, size_t capacity);
bool voice_timing_needs_visible(const char *request_id);

struct VoiceTimingArea { int32_t x1, y1, x2, y2; };
struct VoiceTimingPage {
    const char *request_id;
    uint32_t generation;
    uint16_t page_index;
    VoiceTimingArea area;
    bool visible;
};
// Display functions run on the LVGL task. The token rejects stale DMA results.
uint32_t voice_timing_frame_begin(const VoiceTimingPage &page);
void voice_timing_frame_flush(uint32_t token, const VoiceTimingArea &area,
                              bool first, bool last, bool success, bool still_visible,
                              int64_t completed_us = 0);
extern "C" int64_t voice_timing_now_us();
