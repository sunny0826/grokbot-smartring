#pragma once
#include "voice.h"
#include "voice_page.h"
enum class VoiceRequest { Upload, Status, Retry, Inbox, Read };
struct VoiceReply {
    bool ok = false;
    VoicePhase phase = VoicePhase::UploadFailed;
    char request_id[33] = {};
    uint16_t page_count = 0;
    bool read = false;
    char error[48] = {};
};
VoiceReply voice_decode(const char *body, const char *expected_id);
VoiceReply voice_exchange(VoiceRequest request, const char *request_id,
                         const int16_t *samples = nullptr, size_t count = 0);
uint8_t *voice_download_page(const char *request_id, uint16_t index, uint16_t count);
