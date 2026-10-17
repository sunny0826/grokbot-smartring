#include "voice_mailbox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <algorithm>

namespace {
QueueHandle_t requests;
QueueHandle_t results;
void (*finished)();

void worker(void *)
{
    VoiceMailboxResult result;
    for (;;) {
        if (xQueueReceive(requests, &result.work, portMAX_DELAY) != pdTRUE) continue;
        const auto &work = result.work;
        result.reply = voice_exchange(work.operation, work.request.id, nullptr, 0,
            &work.request.scope, work.operation == VoiceRequest::Inbox ? &work.excluded : nullptr);
        finished();
        while (xQueueSend(results, &result, pdMS_TO_TICKS(50)) != pdTRUE) {}
    }
}
}

bool voice_mailbox_start(void (*network_finished)())
{
    finished = network_finished;
    requests = xQueueCreate(1, sizeof(VoiceMailboxWork));
    results = xQueueCreate(1, sizeof(VoiceMailboxResult));
    return requests && results && xTaskCreate(worker, "voice_mailbox", 8192, nullptr, 3, nullptr) == pdPASS;
}

bool voice_mailbox_submit(const VoiceMailboxWork &work)
{
    return requests && xQueueSend(requests, &work, 0) == pdTRUE;
}

bool voice_mailbox_receive(VoiceMailboxResult &result)
{
    return results && xQueueReceive(results, &result, 0) == pdTRUE;
}

bool VoiceMailboxSync::receive(VoiceJournal &journal, const VoiceScope &scope,
    VoicePhase phase, uint32_t generation, uint32_t epoch, int64_t now, char (&inbox_id)[33])
{
    VoiceMailboxResult result;
    if (!voice_mailbox_receive(result)) return false;
    const auto &work = result.work;
    if (work.operation == VoiceRequest::Read) {
        if (result.reply.ok && result.reply.read && !std::strcmp(result.reply.request_id, work.request.id) &&
            !journal.confirm_read(work.request)) result.reply.ok = false;
        completed(result, now);
    } else {
        completed(result, now);
        if (!result.reply.ok) return false;
        if (phase == VoicePhase::Idle && work.config_epoch == epoch &&
               work.request.scope == scope && work.generation == generation && work.revision == journal.revision()) {
            VoiceReceipt found{scope};
            std::strcpy(found.id, result.reply.request_id);
            const bool unread = result.reply.phase == VoicePhase::Replied && !journal.contains(found);
            std::strcpy(inbox_id, unread ? found.id : "");
            return true;
        }
        refresh();
    }
    return false;
}

bool VoiceMailboxSync::next(const VoiceJournal &journal, const VoiceScope &scope,
    VoicePhase phase, uint32_t generation, uint32_t epoch, int64_t now, VoiceMailboxWork &work) const
{
    if (busy_ || phase == VoicePhase::Recording || phase == VoicePhase::Preparing) return false;
    const VoiceReceipt *receipt = nullptr;
    if (now >= next_read_) for (size_t n = 0; n < journal.size(); ++n) {
        const auto &entry = journal.at((cursor_ + n) % journal.size());
        if (entry.scope == scope) { receipt = &entry; break; }
    }
    const bool inbox = phase == VoicePhase::Idle && now >= next_inbox_;
    if (!receipt && !inbox) return false;
    work = {};
    work.generation = generation;
    work.revision = journal.revision();
    work.config_epoch = epoch;
    if (receipt && (!inbox || !last_read_)) {
        work.operation = VoiceRequest::Read;
        work.request = *receipt;
    } else {
        work.operation = VoiceRequest::Inbox;
        work.request.scope = scope;
        work.excluded = journal.exclusions(scope);
    }
    return true;
}

void VoiceMailboxSync::submitted(const VoiceMailboxWork &work)
{
    busy_ = true;
    last_read_ = work.operation == VoiceRequest::Read;
    if (last_read_) ++cursor_;
}

void VoiceMailboxSync::completed(const VoiceMailboxResult &result, int64_t now)
{
    busy_ = false;
    if (result.work.operation == VoiceRequest::Inbox) next_inbox_ = now + 5000000;
    else {
        const bool confirmed = result.reply.ok && result.reply.read &&
                               !std::strcmp(result.reply.request_id, result.work.request.id);
        failures_ = confirmed ? 0 : std::min(failures_ + 1, 4u);
        next_read_ = now + (confirmed ? 100000 : (int64_t(2) << failures_) * 1000000);
        next_inbox_ = 0;
    }
}
