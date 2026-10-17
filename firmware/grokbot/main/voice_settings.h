#pragma once
#include "voice.h"

enum class VoiceSettingsWork { None, Input, Poll };

class VoiceSettingsGate {
public:
    bool request(uint32_t operation, VoicePhase phase, bool input_pending)
    {
        if (!operation) return false;
        if (owner_) return owner_ == operation;
        const bool network_action = phase == VoicePhase::Uploading || phase == VoicePhase::LoadingPage;
        if ((work_ == VoiceSettingsWork::Input && !network_action) || !safe(phase, input_pending)) return false;
        owner_ = operation;
        status_ = VoiceSettingsStatus::Pending;
        return true;
    }

    bool boundary(VoicePhase phase, bool input_pending)
    {
        if (work_ != VoiceSettingsWork::None || mailbox_active_) return false;
        if (status_ == VoiceSettingsStatus::Pending)
            status_ = safe(phase, input_pending) ? VoiceSettingsStatus::Paused : VoiceSettingsStatus::Busy;
        return status_ == VoiceSettingsStatus::Paused;
    }

    void resume(uint32_t operation)
    {
        if (owner_ == operation) { owner_ = 0; status_ = VoiceSettingsStatus::Inactive; }
    }

    VoiceSettingsStatus status(uint32_t operation) const
    {
        return owner_ == operation && operation ? status_ : VoiceSettingsStatus::Inactive;
    }

    bool blocks_input() const { return owner_ != 0; }

    bool begin_work(VoiceSettingsWork work)
    {
        if (owner_ || work_ != VoiceSettingsWork::None || work == VoiceSettingsWork::None) return false;
        work_ = work;
        return true;
    }

    void finish_work() { work_ = VoiceSettingsWork::None; }

    bool begin_mailbox()
    {
        if (owner_ || mailbox_active_) return false;
        mailbox_active_ = true;
        return true;
    }

    void finish_mailbox() { mailbox_active_ = false; }

private:
    static bool safe(VoicePhase phase, bool input_pending)
    {
        return !input_pending && phase != VoicePhase::Recording && phase != VoicePhase::Preparing;
    }
    uint32_t owner_ = 0;
    VoiceSettingsStatus status_ = VoiceSettingsStatus::Inactive;
    VoiceSettingsWork work_ = VoiceSettingsWork::None;
    bool mailbox_active_ = false;
};
