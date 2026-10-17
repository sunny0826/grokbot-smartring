#pragma once
#include "voice_receipt.h"
#include "voice_transport.h"
#include "voice_journal.h"

struct VoiceMailboxWork {
    VoiceRequest operation = VoiceRequest::Inbox;
    VoiceReceipt request{};
    VoiceReadExclusions excluded{};
    uint32_t generation = 0;
    uint32_t revision = 0;
    uint32_t config_epoch = 0;
    VoicePhase after = VoicePhase::Stored;
};
struct VoiceMailboxResult {
    VoiceMailboxWork work{};
    VoiceReply reply{};
    uint8_t *page = nullptr;
};

bool voice_mailbox_start(void (*network_finished)());
bool voice_mailbox_submit(const VoiceMailboxWork &work);
bool voice_mailbox_receive(VoiceMailboxResult &result);

class VoiceMailboxSync {
public:
    bool receive(VoiceJournal &journal, const VoiceScope &scope, VoicePhase phase,
                 uint32_t generation, uint32_t epoch, int64_t now, char (&inbox_id)[33],
                 VoiceMailboxResult *response = nullptr);
    bool next(const VoiceJournal &journal, const VoiceScope &scope, VoicePhase phase,
              uint32_t generation, uint32_t epoch, int64_t now, VoiceMailboxWork &work) const;
    void submitted(const VoiceMailboxWork &work);
    void completed(const VoiceMailboxResult &result, int64_t now);
    void refresh() { next_inbox_ = 0; }
    bool available() const { return !busy_; }
private:
    bool busy_ = false;
    bool last_read_ = false;
    size_t cursor_ = 0;
    unsigned failures_ = 0;
    int64_t next_read_ = 0;
    int64_t next_inbox_ = 0;
};
