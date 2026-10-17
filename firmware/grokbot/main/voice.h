#pragma once
#include <stddef.h>
#include <stdint.h>

enum class VoicePhase {
    Idle, Recording, Ready, Uploading, UploadFailed, Stored, Transcribing,
    Waiting, Claimed, Replied, NeedsReview, Failed, Unavailable,
    LoadingPage, ViewingReply, ReplyFailed, MarkingRead, Preparing
};
enum class VoiceAction { Boot, Discard, Close, Retry, Open, Previous, Next, MarkRead };
struct VoiceSnapshot {
    VoicePhase phase = VoicePhase::Idle;
    uint32_t generation = 0;
    uint32_t duration_ms = 0;
    uint16_t peak = 0;
    uint16_t page_index = 0;
    uint16_t page_count = 0;
    bool unread = false;
    uint32_t hold_id = 0;
    char error[48] = {};
};
void voice_start();
struct VoiceDiagnostics {
    bool boot_pressed;
    uint32_t presses, queued, handled, start_stage;
};
VoiceDiagnostics voice_diagnostics();
enum class VoiceHoldEnd { Send, Cancel };
uint32_t voice_hold_begin();
void voice_hold_end(uint32_t id, VoiceHoldEnd outcome);
VoiceSnapshot voice_snapshot();
bool voice_action(VoiceAction action, uint32_t generation);
bool voice_copy_page(uint32_t generation, uint8_t *destination, size_t size);

enum class VoiceSettingsStatus { Inactive, Pending, Paused, Busy };
bool voice_settings_pause(uint32_t operation);
VoiceSettingsStatus voice_settings_status(uint32_t operation);
void voice_settings_resume(uint32_t operation);
