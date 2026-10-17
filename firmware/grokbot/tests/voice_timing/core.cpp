#include "voice_timing.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static int64_t now = 1000;
extern "C" int64_t voice_timing_now_us() { return now; }
static constexpr const char *A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static constexpr const char *B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static constexpr VoiceTimingArea bounds{44, 96, 315, 263};
static VoiceTimingRecord record(const char *id) {
    VoiceTimingRecord records[VOICE_TIMING_CAPACITY];
    const auto count = voice_timing_snapshot(records, VOICE_TIMING_CAPACITY);
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(records[i].request_id, id)) return records[i];
    assert(false && "timing start must be observable"); return {};
}
static int64_t visible(const char *id) {
    return record(id).at_us[static_cast<size_t>(VoiceTimingStage::FirstVisible)];
}
static uint32_t begin(const char *id = A, bool shown = true, uint32_t generation = 7) {
    return voice_timing_frame_begin({id, generation, 0, bounds, shown});
}
static void whole(uint32_t token, bool shown = true) {
    now += 100;
    voice_timing_frame_flush(token, bounds, true, true, true, shown);
}
int main() {
    voice_timing_start(A, 900);
    assert(record(A).released_us == 900);
    voice_timing_mark(A, VoiceTimingStage::UploadStarted);
    now = 1100;
    voice_timing_mark(A, VoiceTimingStage::UploadStarted);
    assert(record(A).at_us[0] == 1000);
    whole(begin(A, false)); assert(!visible(A));
    whole(begin(), false); assert(!visible(A));
    auto old = begin(); auto current = begin(A, true, 8);
    whole(old); assert(!visible(A));
    voice_timing_frame_flush(current, {44, 96, 180, 263}, true, true, true, true);
    assert(!visible(A));
    current = begin();
    voice_timing_frame_flush(current, {181, 96, 315, 263}, true, true, true, true);
    assert(!visible(A)); // Never combine partial coverage across refreshes.
    for (int failure = 0; failure < 3; ++failure) {
        current = begin();
        for (int strip = 0; strip < 3; ++strip)
            voice_timing_frame_flush(current, {44, 96 + strip * 56, 315, 151 + strip * 56},
                                     strip == 0, strip == 2, strip != failure, true);
        assert(!visible(A));
    }
    current = begin();
    voice_timing_frame_flush(current, bounds, false, true, true, true);
    assert(!visible(A)); // Missing the first strip is not a complete frame.
    current = begin(A, true, 9);
    voice_timing_frame_flush(current, {44, 96, 179, 263}, true, false, true, true);
    assert(!visible(A));
    now += 500;
    voice_timing_frame_flush(current, {180, 96, 315, 263}, false, true, true, true);
    assert(visible(A) == now && record(A).generation == 9);
    const auto first = now;
    whole(begin()); assert(visible(A) == first);
    voice_timing_start(A, now); assert(record(A).released_us == 900);
    voice_timing_start(B, 0);
    whole(begin(B)); assert(!visible(B));
    voice_timing_mark(B, VoiceTimingStage::FirstVisible); assert(!visible(B));
    for (unsigned i = 0; i < 30; ++i) {
        char id[33]; std::snprintf(id, sizeof(id), "%032x", i);
        voice_timing_start(id, 900);
    }
    VoiceTimingRecord records[VOICE_TIMING_CAPACITY + 1];
    assert(voice_timing_snapshot(records, VOICE_TIMING_CAPACITY + 1) == VOICE_TIMING_CAPACITY);
    assert(voice_timing_snapshot(nullptr, 0) == 0);
    std::puts("PASS timing core: coverage, all strip failures, stale frames, no release, once, bounded");
}
