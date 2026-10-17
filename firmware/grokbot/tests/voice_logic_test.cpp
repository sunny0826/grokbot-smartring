#include "voice_logic.h"
#include "voice_page.h"
#include <vector>
#include <cassert>
#include <cstdio>

int main()
{
    VoiceHoldControl hold;
    assert(hold.begin(false) == 0);
    auto first = hold.begin(true);
    assert(first && !hold.begin(true));
    assert(voice_hold_intent(hold.snapshot(), 0, VoicePhase::Idle) == VoiceHoldIntent::Start);
    hold.finish(first, VoiceHoldEnd::Send);
    // Letting go while the actor is busy must not start an unattended recording.
    assert(voice_hold_intent(hold.snapshot(), 0, VoicePhase::Idle) == VoiceHoldIntent::Clear);
    hold.clear(first);
    auto second = hold.begin(true);
    assert(second != first);
    hold.finish(first, VoiceHoldEnd::Cancel);
    assert(voice_hold_intent(hold.snapshot(), second, VoicePhase::Recording) == VoiceHoldIntent::None);
    assert(voice_hold_intent(hold.snapshot(), second, VoicePhase::Ready) == VoiceHoldIntent::None);
    hold.finish(second, VoiceHoldEnd::Cancel);
    hold.finish(second, VoiceHoldEnd::Send);
    assert(voice_hold_intent(hold.snapshot(), second, VoicePhase::Recording) == VoiceHoldIntent::Discard);
    hold.clear(first);
    assert(voice_hold_intent(hold.snapshot(), second, VoicePhase::Ready) == VoiceHoldIntent::Discard);
    hold.clear(second);
    auto third = hold.begin(true);
    hold.finish(third, VoiceHoldEnd::Send);
    assert(voice_hold_intent(hold.snapshot(), third, VoicePhase::Recording) == VoiceHoldIntent::Send);
    assert(voice_hold_intent(hold.snapshot(), third, VoicePhase::Ready) == VoiceHoldIntent::Send);
    assert(voice_hold_intent(hold.snapshot(), third, VoicePhase::Unavailable) == VoiceHoldIntent::Clear);
    hold.clear(third);
    assert(voice_hold_intent(hold.snapshot(), 0, VoicePhase::Idle) == VoiceHoldIntent::None);
    VoiceButton button;
    assert(!button.update(true, 0));
    assert(!button.update(true, 100));  // Held during startup never starts recording.
    assert(!button.update(false, 110));
    assert(!button.update(false, 150));
    assert(!button.update(true, 160));
    assert(!button.update(false, 170));
    assert(!button.update(true, 180));
    assert(!button.update(true, 210));
    assert(button.update(true, 220));
    assert(!button.update(true, 500));
    assert(!button.update(false, 510));
    assert(!button.update(false, 550));
    assert(!button.update(true, 600));
    assert(button.update(true, 640));
    assert(voice_intent(VoicePhase::Idle, VoiceAction::Boot) == VoiceIntent::Start);
    assert(voice_intent(VoicePhase::Recording, VoiceAction::Boot) == VoiceIntent::Send);
    assert(voice_intent(VoicePhase::Ready, VoiceAction::Boot) == VoiceIntent::Send);
    assert(voice_intent(VoicePhase::Uploading, VoiceAction::Boot) == VoiceIntent::None);
    assert(voice_intent(VoicePhase::Uploading, VoiceAction::Close) == VoiceIntent::None);
    assert(voice_intent(VoicePhase::UploadFailed, VoiceAction::Boot) == VoiceIntent::RetryUpload);
    assert(voice_intent(VoicePhase::NeedsReview, VoiceAction::Retry) == VoiceIntent::None);
    assert(voice_intent(VoicePhase::Replied, VoiceAction::Retry) == VoiceIntent::None);
    assert(voice_intent(VoicePhase::Stored, VoiceAction::Discard) == VoiceIntent::None);
    assert(!voice_has_audio(VOICE_RATE / 10, 1000));
    assert(!voice_has_audio(VOICE_RATE, 0));
    assert(voice_has_audio(VOICE_RATE, 1000));
    for (VoicePhase phase : {VoicePhase::Recording, VoicePhase::Ready, VoicePhase::Uploading,
             VoicePhase::LoadingPage, VoicePhase::MarkingRead}) {
        assert(voice_intent(phase, VoiceAction::Open) == VoiceIntent::None);
        assert(voice_intent(phase, VoiceAction::MarkRead) == VoiceIntent::None);
    }
    assert(voice_intent(VoicePhase::Idle, VoiceAction::Open) == VoiceIntent::Open);
    assert(voice_intent(VoicePhase::ViewingReply, VoiceAction::Close) == VoiceIntent::Close);
    assert(voice_intent(VoicePhase::ViewingReply, VoiceAction::Next) == VoiceIntent::Next);
    assert(voice_intent(VoicePhase::ViewingReply, VoiceAction::MarkRead) == VoiceIntent::MarkRead);
    std::vector<uint8_t> page(VOICE_PAGE_HEADER + VOICE_PAGE_BYTES, 0);
    memcpy(page.data(), "VPG1", 4);
    auto set = [&](int offset, uint32_t value, int n) {
        for (int i=0; i<n; ++i) page[offset+i] = value >> (i*8);
    };
    set(4, 272, 2); set(6, 168, 2); set(10, 2, 2);
    // CRC of 91392 zero bytes, independently computed with Python zlib.
    set(12, 0xdf43ac4f, 4);
    assert(voice_page_valid(page.data(), page.size(), 0, 2));
    assert(!voice_page_valid(page.data(), page.size()-1, 0, 2));
    assert(!voice_page_valid(page.data(), page.size(), 1, 2));
    assert(!voice_page_valid(page.data(), page.size(), 0, 3));
    assert(!voice_page_valid(page.data(), page.size(), 0, 9));
    page[20] = 1; assert(!voice_page_valid(page.data(), page.size(), 0, 2)); page[20] = 0;
    page[4] = 0; assert(!voice_page_valid(page.data(), page.size(), 0, 2));
    VoiceSnapshot current;
    current.generation = 9;
    assert(voice_input_current(VoiceAction::Boot, VoicePhase::Idle, 8, current));
    assert(!voice_input_current(VoiceAction::Open, VoicePhase::Idle, 8, current));
    assert(!voice_input_current(VoiceAction::Boot, VoicePhase::Uploading, 8, current));
    current.phase = VoicePhase::Recording;
    assert(!voice_input_current(VoiceAction::Boot, VoicePhase::Idle, 8, current));
    uint8_t header[44];
    voice_wav_header(header, VOICE_RATE);
    auto le = [&](int offset) {
        return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
               uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24;
    };
    assert(!memcmp(header, "RIFF", 4));
    assert(!memcmp(header + 8, "WAVEfmt ", 8));
    assert(le(4) == 96036 && le(24) == 48000 && le(40) == 96000);
    assert(header[20] == 1 && header[22] == 1 && header[34] == 16);
    puts("Hold/release/cancel ownership, BOOT debounce, record/send states, silence gate and WAV contract passed");
}
