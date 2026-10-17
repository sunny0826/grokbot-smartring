#include "voice.h"
#include "voice_logic.h"
#include "voice_settings.h"
#include "voice_transport.h"
#include "voice_journal.h"
#include "voice_mailbox.h"
#include "voice_scope.h"
#include "voice_timing.h"
#include "voice_button.h"
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
struct Input { VoiceAction action; uint32_t generation; VoicePhase origin; };
QueueHandle_t inputs;
SemaphoreHandle_t snapshot_lock;
VoiceSnapshot published;
VoiceSnapshot current;
int16_t *audio;
int16_t *scratch;
size_t samples;
uint16_t recording_peak;
char request_id[33];
bool codec_ready;
VoiceJournal journal;
VoiceMailboxSync mailbox_sync;
VoiceScope mailbox_scope{};
VoiceScope request_scope{};
std::atomic<uint32_t> config_epoch{1};
uint32_t loaded_epoch;
bool scope_ready;
int64_t next_poll;
char inbox_id[33];
uint8_t *page_buffer;
VoiceHoldControl hold_control;
VoiceSettingsGate settings_gate;

void publish()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    strlcpy(current.request_id, request_id, sizeof(current.request_id));
    published = current;
    xSemaphoreGive(snapshot_lock);
}

void update_phase(VoicePhase value, const char *error = nullptr)
{
    if (current.phase != value) ++current.generation;
    current.phase = value;
    strlcpy(current.error, error ? error : "", sizeof(current.error));
}

void phase(VoicePhase value, const char *error = nullptr)
{
    update_phase(value, error);
    publish();
}

void release_audio()
{
    free(audio);
    audio = nullptr;
    free(scratch);
    scratch = nullptr;
    samples = 0;
}

void close()
{
    current.hold_id = 0;
    if (!journal.clear_pending()) {
        phase(VoicePhase::Unavailable, "storage_unavailable");
        return;
    }
    release_audio();
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    free(page_buffer);
    page_buffer = nullptr;
    xSemaphoreGive(snapshot_lock);
    request_id[0] = 0;
    current.page_count = current.page_index = 0;
    next_poll = 0;
    mailbox_sync.refresh();
    current.duration_ms = current.peak = 0;
    phase(VoicePhase::Idle);
}

void begin()
{
    voice_http_invalidate();
    phase(VoicePhase::Preparing);
    voice_recording_stage(1);
    release_audio();
    audio = static_cast<int16_t *>(heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    scratch = static_cast<int16_t *>(heap_caps_malloc(960 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!audio || !scratch) {
        release_audio();
        phase(VoicePhase::Unavailable, "audio_memory");
        return;
    }
    if (!codec_ready) {
        voice_recording_stage(2);
        codec_ready = bsp_audio_init() == ESP_OK;
        if (codec_ready) codec_ready = esp_codec_dev_set_in_gain(bsp_audio_get_codec(), 36.f) == 0;
    }
    if (!codec_ready) {
        release_audio();
        phase(VoicePhase::Unavailable, "microphone_unavailable");
        return;
    }
    voice_recording_stage(3);
    for (int i = 0; i < 32; ++i) {
        size_t discarded = 0;
        if (i2s_channel_read(bsp_audio_get_rx(), scratch, 960 * 4, &discarded, 0) != ESP_OK || !discarded) break;
    }
    for (int i = 0; i < 4; ++i) snprintf(request_id + i * 8, 9, "%08lx", (unsigned long)esp_random());
    request_scope = mailbox_scope;
    recording_peak = 0;
    current.duration_ms = current.peak = 0;
    phase(VoicePhase::Recording);
    voice_recording_stage(4);
}

void update_reply(const VoiceReply &reply)
{
    if (!reply.ok) {
        update_phase(VoicePhase::UploadFailed, reply.error);
        return;
    }
    release_audio();
    current.page_count = reply.page_count;
    if (reply.phase == VoicePhase::Replied && !reply.read) {
        strlcpy(inbox_id, request_id, sizeof(inbox_id));
        current.unread = true;
    }
    update_phase(reply.phase, reply.error);
    next_poll = esp_timer_get_time() + 250000;
}

void accept_reply(const VoiceReply &reply)
{
    update_reply(reply);
    publish();
}

void send(int64_t released_us)
{
    if (!voice_has_audio(samples, recording_peak)) {
        release_audio();
        phase(VoicePhase::Unavailable, "empty_recording");
        return;
    }
    voice_timing_start(request_id, released_us);
    VoiceReceipt pending{request_scope};
    strlcpy(pending.id, request_id, sizeof(pending.id));
    if (!scope_ready || !journal.set_pending(pending)) {
        phase(VoicePhase::Ready, "storage_unavailable");
        return;
    }
    voice_timing_mark(request_id, VoiceTimingStage::UploadStarted);
    phase(VoicePhase::Uploading);
    accept_reply(voice_exchange(VoiceRequest::Upload, request_id, audio, samples, &request_scope));
}

void update_page(uint8_t *downloaded)
{
    free(page_buffer);
    page_buffer = downloaded;
    ++current.generation;
    current.phase = VoicePhase::ViewingReply;
    current.error[0] = 0;
}

void show_page(uint8_t *downloaded)
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    update_page(downloaded);
    published = current;
    xSemaphoreGive(snapshot_lock);
}

void load_page(uint16_t index)
{
    current.page_index = index;
    phase(VoicePhase::LoadingPage);
    auto *downloaded = voice_download_page(request_id, index, current.page_count, &request_scope);
    if (downloaded) show_page(downloaded);
    else phase(VoicePhase::ReplyFailed, "page_download_failed");
}

void open_reply()
{
    if (current.phase == VoicePhase::Idle || !current.page_count) {
        if (current.phase == VoicePhase::Idle) {
            if (!current.unread || !inbox_id[0]) return;
            strlcpy(request_id, inbox_id, sizeof(request_id));
            request_scope = mailbox_scope;
        }
        phase(VoicePhase::LoadingPage);
        auto reply = voice_exchange(VoiceRequest::Status, request_id, nullptr, 0, &request_scope);
        if (!reply.ok || reply.phase != VoicePhase::Replied) {
            phase(VoicePhase::ReplyFailed, "reply_unavailable"); return;
        }
        current.page_count = reply.page_count;
        current.page_index = 0;
    }
    load_page(current.page_index);
}

void mark_read()
{
    if (current.page_index + 1 != current.page_count) return;
    VoiceReceipt receipt{request_scope};
    strlcpy(receipt.id, request_id, sizeof(receipt.id));
    if (journal.mark_read(receipt) != VoiceJournalResult::Saved) {
        phase(VoicePhase::ViewingReply, "read_save_failed");
        return;
    }
    current.unread = false;
    inbox_id[0] = 0;
    close();
}

void handle(const Input &input)
{
    if (input.action == VoiceAction::Boot && current.hold_id) return;
    if (!voice_input_current(input.action, input.origin, input.generation, current)) return;
    if (input.action == VoiceAction::Boot) voice_button_handled();
    switch (voice_intent(current.phase, input.action)) {
    case VoiceIntent::Open: open_reply(); break;
    case VoiceIntent::Previous:
        if (current.page_index) load_page(current.page_index - 1);
        break;
    case VoiceIntent::Next:
        if (current.page_index + 1 < current.page_count) load_page(current.page_index + 1);
        break;
    case VoiceIntent::MarkRead: mark_read(); break;
    case VoiceIntent::Start: begin(); break;
    case VoiceIntent::Send: send(0); break;
    case VoiceIntent::RetryUpload:
        phase(VoicePhase::Uploading);
        accept_reply(audio ? voice_exchange(VoiceRequest::Upload, request_id, audio, samples, &request_scope) :
                            voice_exchange(VoiceRequest::Status, request_id, nullptr, 0, &request_scope));
        break;
    case VoiceIntent::RetryRemote:
        phase(VoicePhase::Uploading);
        accept_reply(voice_exchange(VoiceRequest::Retry, request_id, nullptr, 0, &request_scope));
        break;
    case VoiceIntent::Discard:
    case VoiceIntent::Close: close(); break;
    case VoiceIntent::None: break;
    }
}

void handle_hold()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    auto request = hold_control.snapshot();
    xSemaphoreGive(snapshot_lock);
    auto intent = voice_hold_intent(request, current.hold_id, current.phase);
    if (intent == VoiceHoldIntent::None) return;
    if (intent == VoiceHoldIntent::Start) {
        current.hold_id = request.id;
        begin();
        return;
    }
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    hold_control.clear(request.id);
    xSemaphoreGive(snapshot_lock);
    current.hold_id = 0;
    if (intent == VoiceHoldIntent::Send && samples >= VOICE_RATE * 3 / 10) send(request.released_us);
    else if (intent == VoiceHoldIntent::Discard || intent == VoiceHoldIntent::Send) close();
    else publish();
}

void capture()
{
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(bsp_audio_get_rx(), scratch, 960 * 4, &bytes, 50);
    if (err != ESP_OK || bytes == 0 || bytes % 4) {
        release_audio();
        phase(VoicePhase::Unavailable, "microphone_read_failed");
        return;
    }
    uint16_t peak = 0;
    for (size_t i = 0; i < bytes / 4 && samples < VOICE_MAX_SAMPLES; ++i) {
        int sample = scratch[i * 2];
        audio[samples++] = (int16_t)sample;
        peak = std::max(peak, (uint16_t)(sample < 0 ? -sample : sample));
    }
    recording_peak = std::max(recording_peak, peak);
    current.peak = peak;
    current.duration_ms = samples * 1000 / VOICE_RATE;
    if (samples == VOICE_MAX_SAMPLES) phase(VoicePhase::Ready);
    else publish();
}

bool begin_poll()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const bool accepted = settings_gate.begin_work(VoiceSettingsWork::Poll);
    xSemaphoreGive(snapshot_lock);
    return accepted;
}

void finish_work()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    settings_gate.finish_work();
    xSemaphoreGive(snapshot_lock);
}

void mailbox_finished()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    settings_gate.finish_mailbox();
    xSemaphoreGive(snapshot_lock);
}

void mailbox_step()
{
    if (current.phase == VoicePhase::Recording || current.phase == VoicePhase::Preparing) return;
    char found[33]{};
    VoiceMailboxResult response;
    if (mailbox_sync.receive(journal, mailbox_scope, current.phase, current.generation,
        config_epoch.load(), esp_timer_get_time(), found, &response)) {
        const bool unread = found[0];
        if (current.unread != unread || strcmp(inbox_id, found)) ++current.generation;
        current.unread = unread;
        strlcpy(inbox_id, found, sizeof(inbox_id));
        publish();
    }
    if (response.work.operation == VoiceRequest::Response) {
        const auto &work = response.work;
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        if (!settings_gate.blocks_input() && !uxQueueMessagesWaiting(inputs) && !hold_control.snapshot().id &&
            work.config_epoch == config_epoch.load() && work.generation == current.generation &&
            work.request.scope == request_scope && !strcmp(work.request.id, request_id) &&
            (!response.reply.ok || !strcmp(response.reply.request_id, request_id))) {
            update_reply(response.reply);
            if (response.reply.ok && response.reply.phase == VoicePhase::Replied && response.page) {
                current.page_index = 0;
                update_page(response.page);
                response.page = nullptr;
            }
            published = current;
        }
        xSemaphoreGive(snapshot_lock);
        free(response.page);
    }
    VoiceMailboxWork work;
    if (!scope_ready) return;
    if (!mailbox_sync.next(journal, mailbox_scope, current.phase,
        current.generation, config_epoch.load(), esp_timer_get_time(), work)) {
        if (!mailbox_sync.available() || !voice_waiting(current.phase) || esp_timer_get_time() < next_poll) return;
        work.operation = VoiceRequest::Response;
        work.request.scope = request_scope;
        strlcpy(work.request.id, request_id, sizeof(work.request.id));
        work.generation = current.generation;
        work.config_epoch = config_epoch.load();
        work.after = current.phase;
    }
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const bool reserved = !hold_control.snapshot().id && !uxQueueMessagesWaiting(inputs) &&
                          settings_gate.begin_mailbox();
    xSemaphoreGive(snapshot_lock);
    if (!reserved) return;
    if (voice_mailbox_submit(work)) mailbox_sync.submitted(work);
    else mailbox_finished();
}

void worker(void *)
{
    scope_ready = voice_current_scope(mailbox_scope);
    loaded_epoch = config_epoch.load();
    if (!journal.load(mailbox_scope)) { phase(VoicePhase::Unavailable, "storage_unavailable"); vTaskDelete(nullptr); }
    bool recover = journal.pending().id[0] && journal.pending().scope == mailbox_scope && scope_ready;
    if (recover) {
        strlcpy(request_id, journal.pending().id, sizeof(request_id));
        request_scope = journal.pending().scope;
    }
    if (recover) phase(VoicePhase::Uploading);
    for (;;) {
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        const bool paused = settings_gate.boundary(current.phase,
            current.hold_id || hold_control.snapshot().id || uxQueueMessagesWaiting(inputs));
        xSemaphoreGive(snapshot_lock);
        if (paused) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        if (recover && begin_poll()) {
            accept_reply(voice_exchange(VoiceRequest::Status, request_id, nullptr, 0, &request_scope));
            finish_work();
            recover = false;
        }
        if (loaded_epoch != config_epoch.load()) {
            loaded_epoch = config_epoch.load();
            const auto previous = mailbox_scope;
            scope_ready = voice_current_scope(mailbox_scope);
            if (!scope_ready || previous != mailbox_scope) {
                current.unread = false;
                inbox_id[0] = 0;
                ++current.generation;
                publish();
            }
            mailbox_sync.refresh();
        }
        handle_hold();
        Input input;
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        const bool received = !settings_gate.blocks_input() && xQueueReceive(inputs, &input, 0) == pdTRUE;
        if (received) settings_gate.begin_work(VoiceSettingsWork::Input);
        xSemaphoreGive(snapshot_lock);
        if (received) {
            handle(input);
            finish_work();
        } else if (current.phase != VoicePhase::Recording) vTaskDelay(pdMS_TO_TICKS(30));
        if (current.phase == VoicePhase::Recording) capture();
        mailbox_step();
    }
}

}

uint32_t voice_hold_begin()
{
    if (!snapshot_lock) return 0;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    uint32_t id = hold_control.begin(!settings_gate.blocks_input() && published.phase == VoicePhase::Idle);
    xSemaphoreGive(snapshot_lock);
    return id;
}

void voice_hold_end(uint32_t id, VoiceHoldEnd outcome)
{
    if (!snapshot_lock) return;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    hold_control.finish(id, outcome, esp_timer_get_time());
    xSemaphoreGive(snapshot_lock);
}

VoiceSnapshot voice_snapshot()
{
    if (!snapshot_lock) return {};
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    auto copy = published;
    xSemaphoreGive(snapshot_lock);
    return copy;
}

bool voice_copy_page(uint32_t generation, uint8_t *destination, size_t size)
{
    if (!snapshot_lock || size != VOICE_PAGE_BYTES) return false;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    bool valid = page_buffer && published.phase == VoicePhase::ViewingReply &&
                 published.generation == generation;
    if (valid) memcpy(destination, page_buffer + VOICE_PAGE_HEADER, size);
    xSemaphoreGive(snapshot_lock);
    return valid;
}

bool voice_action(VoiceAction action, uint32_t generation)
{
    if (!snapshot_lock || !inputs) return false;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    Input input = {action, generation, published.phase};
    const bool accepted = !settings_gate.blocks_input() && published.generation == generation &&
                          xQueueSend(inputs, &input, 0) == pdTRUE;
    xSemaphoreGive(snapshot_lock);
    return accepted;
}

bool voice_settings_pause(uint32_t operation)
{
    if (!snapshot_lock || !inputs) return false;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const bool accepted = settings_gate.request(operation, published.phase,
        published.hold_id || hold_control.snapshot().id || uxQueueMessagesWaiting(inputs));
    xSemaphoreGive(snapshot_lock);
    if (accepted) voice_http_invalidate();
    return accepted;
}

VoiceSettingsStatus voice_settings_status(uint32_t operation)
{
    if (!snapshot_lock) return VoiceSettingsStatus::Inactive;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    const auto status = settings_gate.status(operation);
    xSemaphoreGive(snapshot_lock);
    return status;
}

void voice_settings_resume(uint32_t operation)
{
    if (!snapshot_lock) return;
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    if (settings_gate.status(operation) != VoiceSettingsStatus::Inactive) ++config_epoch;
    settings_gate.resume(operation);
    xSemaphoreGive(snapshot_lock);
}

void voice_start()
{
    snapshot_lock = xSemaphoreCreateMutex();
    inputs = xQueueCreate(4, sizeof(Input));
    configASSERT(snapshot_lock && inputs);
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOARD_BOOT_IO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    if (nvs_flash_init() != ESP_OK || gpio_config(&config) != ESP_OK) {
        phase(VoicePhase::Unavailable, "voice_init_failed");
        return;
    }
    if (!voice_mailbox_start(mailbox_finished)) { phase(VoicePhase::Unavailable, "audio_memory"); return; }
    if (xTaskCreate(worker, "voice", 12288, nullptr, 4, nullptr) != pdPASS) {
        phase(VoicePhase::Unavailable, "audio_memory");
        return;
    }
    if (!voice_button_start())
        phase(VoicePhase::Unavailable, "button_unavailable");
}
