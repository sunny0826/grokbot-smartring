#pragma once
#include "voice_receipt.h"

enum class VoiceJournalResult { Saved, Full, StorageError };

class VoiceJournal {
public:
    bool load(const VoiceScope &legacy_scope);
    bool set_pending(const VoiceReceipt &request);
    bool clear_pending();
    VoiceJournalResult mark_read(const VoiceReceipt &request);
    bool confirm_read(const VoiceReceipt &request);
    bool contains(const VoiceReceipt &request) const;
    const VoiceReceipt &pending() const { return state_.pending; }
    size_t size() const { return state_.count; }
    const VoiceReceipt &at(size_t index) const { return state_.reads[index]; }
    uint32_t revision() const { return revision_; }
    VoiceReadExclusions exclusions(const VoiceScope &scope) const;

private:
    struct State {
        VoiceReceipt pending{};
        uint8_t count = 0;
        VoiceReceipt reads[VOICE_READ_LIMIT]{};
    } state_;
    bool save(const State &candidate);
    bool ready_ = false;
    uint32_t revision_ = 0;
};

