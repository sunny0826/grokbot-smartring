#pragma once

#include "voice.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr uint32_t VOICE_RATE = 48000;
constexpr uint32_t VOICE_MAX_SECONDS = 30;
constexpr size_t VOICE_MAX_SAMPLES = VOICE_RATE * VOICE_MAX_SECONDS;

enum class VoiceHoldState { Idle, Holding, Send, Cancel };
struct VoiceHoldRequest { uint32_t id = 0; VoiceHoldState state = VoiceHoldState::Idle; };
class VoiceHoldControl {
public:
    uint32_t begin(bool idle) {
        if (!idle || request_.state != VoiceHoldState::Idle) return 0;
        if (++next_id_ == 0) ++next_id_;
        request_ = {next_id_, VoiceHoldState::Holding};
        return next_id_;
    }
    void finish(uint32_t id, VoiceHoldEnd outcome) {
        if (id && request_.id == id && request_.state == VoiceHoldState::Holding)
            request_.state = outcome == VoiceHoldEnd::Send ? VoiceHoldState::Send : VoiceHoldState::Cancel;
    }
    VoiceHoldRequest snapshot() const { return request_; }
    void clear(uint32_t id) { if (request_.id == id) request_ = {}; }
private:
    VoiceHoldRequest request_;
    uint32_t next_id_ = 0;
};

enum class VoiceHoldIntent { None, Start, Send, Discard, Clear };
inline VoiceHoldIntent voice_hold_intent(VoiceHoldRequest request, uint32_t active, VoicePhase phase)
{
    if (!request.id) return VoiceHoldIntent::None;
    if (!active) return request.state == VoiceHoldState::Holding && phase == VoicePhase::Idle
        ? VoiceHoldIntent::Start : VoiceHoldIntent::Clear;
    if (request.id != active || request.state == VoiceHoldState::Holding) return VoiceHoldIntent::None;
    if (phase != VoicePhase::Recording && phase != VoicePhase::Ready) return VoiceHoldIntent::Clear;
    return request.state == VoiceHoldState::Send ? VoiceHoldIntent::Send : VoiceHoldIntent::Discard;
}

class VoiceButton {
public:
    bool update(bool pressed, uint32_t now_ms) {
        if (pressed != candidate_) {
            candidate_ = pressed;
            changed_at_ = now_ms;
        }
        if (now_ms - changed_at_ < 35) return false;
        if (!pressed) armed_ = true;
        if (stable_ == pressed) return false;
        stable_ = pressed;
        if (!pressed || !armed_) return false;
        armed_ = false;
        return true;
    }
private:
    bool candidate_ = false;
    bool stable_ = false;
    bool armed_ = false;
    uint32_t changed_at_ = 0;
};

enum class VoiceIntent { None, Start, Send, RetryUpload, RetryRemote, Discard, Close, Open, Previous, Next, MarkRead };

inline VoiceIntent voice_intent(VoicePhase phase, VoiceAction action)
{
    if (action == VoiceAction::Boot) {
        if (phase == VoicePhase::Idle) return VoiceIntent::Start;
        if (phase == VoicePhase::Recording || phase == VoicePhase::Ready) return VoiceIntent::Send;
        if (phase == VoicePhase::UploadFailed) return VoiceIntent::RetryUpload;
    }
    if (action == VoiceAction::Retry) {
        if (phase == VoicePhase::UploadFailed) return VoiceIntent::RetryUpload;
        if (phase == VoicePhase::Failed)
            return VoiceIntent::RetryRemote;
    }
    if (action == VoiceAction::Open && (phase == VoicePhase::Idle ||
        phase == VoicePhase::Replied || phase == VoicePhase::ReplyFailed)) return VoiceIntent::Open;
    if (phase == VoicePhase::ViewingReply) {
        if (action == VoiceAction::Previous) return VoiceIntent::Previous;
        if (action == VoiceAction::Next) return VoiceIntent::Next;
        if (action == VoiceAction::MarkRead) return VoiceIntent::MarkRead;
    }
    if (action == VoiceAction::Discard &&
        (phase == VoicePhase::Recording || phase == VoicePhase::Ready)) return VoiceIntent::Discard;
    if (action == VoiceAction::Close && phase != VoicePhase::Recording &&
        phase != VoicePhase::Ready && phase != VoicePhase::Uploading &&
        phase != VoicePhase::LoadingPage) return VoiceIntent::Close;
    return VoiceIntent::None;
}

inline bool voice_input_current(VoiceAction action, VoicePhase origin, uint32_t generation,
                                const VoiceSnapshot &current)
{
    return generation == current.generation ||
           (action == VoiceAction::Boot && origin == VoicePhase::Idle && current.phase == VoicePhase::Idle);
}

inline bool voice_has_audio(size_t samples, uint16_t peak)
{
    return samples >= VOICE_RATE * 3 / 10 && peak >= 128;
}

inline void voice_wav_header(uint8_t *header, size_t samples)
{
    memset(header, 0, 44);
    auto le = [&](int offset, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) header[offset + i] = (uint8_t)(value >> (i * 8));
    };
    memcpy(header, "RIFF", 4);
    le(4, 36 + samples * 2, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    le(16, 16, 4);
    le(20, 1, 2);
    le(22, 1, 2);
    le(24, VOICE_RATE, 4);
    le(28, VOICE_RATE * 2, 4);
    le(32, 2, 2);
    le(34, 16, 2);
    memcpy(header + 36, "data", 4);
    le(40, samples * 2, 4);
}
