#include "voice_timing.h"
#include "voice_page.h"
#include <algorithm>
#include <cstring>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#else
#include <chrono>
#include <mutex>
#endif

extern "C" __attribute__((weak)) int64_t voice_timing_now_us()
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

namespace {
VoiceTimingRecord records[VOICE_TIMING_CAPACITY];
size_t next_record;
#ifdef ESP_PLATFORM
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
struct Lock {
    Lock() { portENTER_CRITICAL(&mutex); }
    ~Lock() { portEXIT_CRITICAL(&mutex); }
};
#else
std::mutex mutex;
struct Lock : std::lock_guard<std::mutex> { Lock() : std::lock_guard<std::mutex>(mutex) {} };
#endif
constexpr size_t stage_count = static_cast<size_t>(VoiceTimingStage::Count);
constexpr size_t visible_stage = static_cast<size_t>(VoiceTimingStage::FirstVisible);
struct Frame {
    uint32_t token = 0;
    char request_id[33] = {};
    uint32_t generation = 0;
    uint16_t page_index = 0;
    VoiceTimingArea bounds{};
    bool eligible = false, started = false, failed = false;
    size_t covered = 0;
    uint8_t coverage[(VOICE_PAGE_WIDTH * VOICE_PAGE_HEIGHT + 7) / 8] = {};
} frame;

bool valid_id(const char *id)
{
    if (!id) return false;
    for (size_t i = 0; i < 32; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return id[32] == '\0';
}
VoiceTimingRecord *find(const char *id)
{
    for (auto &record : records)
        if (!std::strcmp(record.request_id, id)) return &record;
    return nullptr;
}
void log_stage(const VoiceTimingRecord &record, size_t stage)
{
#ifdef ESP_PLATFORM
    static const char *names[] = {"upload_started", "connected", "upload_confirmed", "reply_ready", "page_validated", "first_visible"};
    ESP_LOGI("voice_timing", "request_id=%s stage=%s at_us=%lld release_us=%lld elapsed_us=%lld generation=%lu page=%u",
        record.request_id, names[stage], (long long)record.at_us[stage], (long long)record.released_us,
        (long long)(record.released_us ? record.at_us[stage] - record.released_us : -1),
        (unsigned long)record.generation, record.page_index);
#else
    (void)record; (void)stage;
#endif
}
void complete_frame(int64_t completed_us)
{
    VoiceTimingRecord copy;
    {
        Lock lock;
        auto *record = find(frame.request_id);
        if (!record || !record->released_us || record->at_us[visible_stage] || completed_us < record->released_us) return;
        record->at_us[visible_stage] = completed_us;
        record->generation = frame.generation;
        record->page_index = frame.page_index;
        copy = *record;
    }
    log_stage(copy, visible_stage);
}
}

void voice_timing_start(const char *id, int64_t released_us)
{
    if (!valid_id(id)) return;
    Lock lock;
    if (find(id)) return; // Upload retries retain the original physical release.
    auto &record = records[next_record++ % VOICE_TIMING_CAPACITY];
    record = {};
    std::memcpy(record.request_id, id, sizeof(record.request_id));
    const auto now = voice_timing_now_us();
    record.released_us = released_us > 0 && released_us <= now ? released_us : 0;
}

void voice_timing_mark(const char *id, VoiceTimingStage stage)
{
    const auto index = static_cast<size_t>(stage);
    if (!valid_id(id) || index >= stage_count || index == visible_stage) return;
    VoiceTimingRecord copy;
    {
        Lock lock;
        auto *record = find(id);
        if (!record || record->at_us[index]) return;
        record->at_us[index] = voice_timing_now_us();
        copy = *record;
    }
    log_stage(copy, index);
}

size_t voice_timing_snapshot(VoiceTimingRecord *out, size_t capacity)
{
    if (!out) return 0;
    Lock lock;
    size_t count = 0;
    for (const auto &record : records)
        if (record.request_id[0] && count < capacity) out[count++] = record;
    return count;
}

bool voice_timing_needs_visible(const char *id)
{
    if (!valid_id(id)) return false;
    Lock lock;
    const auto *record = find(id);
    return record && record->released_us && !record->at_us[visible_stage];
}

uint32_t voice_timing_frame_begin(const VoiceTimingPage &page)
{
    const uint32_t token = frame.token + 1;
    frame.token = token ? token : 1;
    frame.started = frame.failed = false;
    frame.covered = 0;
    const auto width = int64_t(page.area.x2) - page.area.x1 + 1;
    const auto height = int64_t(page.area.y2) - page.area.y1 + 1;
    frame.eligible = page.visible && voice_timing_needs_visible(page.request_id) &&
        width == VOICE_PAGE_WIDTH && height == VOICE_PAGE_HEIGHT;
    if (frame.eligible) {
        std::memset(frame.coverage, 0, sizeof(frame.coverage));
        std::memcpy(frame.request_id, page.request_id, sizeof(frame.request_id));
        frame.generation = page.generation;
        frame.page_index = page.page_index;
        frame.bounds = page.area;
    }
    return frame.token;
}

void voice_timing_frame_flush(uint32_t token, const VoiceTimingArea &area,
                              bool first, bool last, bool success, bool still_visible, int64_t completed_us)
{
    if (token != frame.token || !frame.eligible) return;
    if (!completed_us) completed_us = voice_timing_now_us();
    if (first) {
        if (frame.started) frame.failed = true;
        frame.started = true;
    }
    frame.failed |= !success || !still_visible || !frame.started;
    if (!frame.failed) {
        const int x1 = std::max(area.x1, frame.bounds.x1), x2 = std::min(area.x2, frame.bounds.x2);
        const int y1 = std::max(area.y1, frame.bounds.y1), y2 = std::min(area.y2, frame.bounds.y2);
        for (int y = y1; y <= y2; ++y) for (int x = x1; x <= x2; ++x) {
            const size_t bit = (y - frame.bounds.y1) * VOICE_PAGE_WIDTH + x - frame.bounds.x1;
            const uint8_t mask = uint8_t(1u << (bit % 8));
            if (!(frame.coverage[bit / 8] & mask)) {
                frame.coverage[bit / 8] |= mask;
                ++frame.covered;
            }
        }
    }
    if (last) {
        if (!frame.failed && frame.covered == VOICE_PAGE_WIDTH * VOICE_PAGE_HEIGHT) complete_frame(completed_us);
        frame.eligible = false;
    }
}
