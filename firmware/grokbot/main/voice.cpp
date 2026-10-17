#include "voice.h"
#include "voice_logic.h"
#include "voice_settings.h"
#include "voice_transport.h"
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
#include "nvs.h"
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
int64_t next_poll;
char inbox_id[33];
uint8_t *page_buffer;
VoiceHoldControl hold_control;
VoiceSettingsGate settings_gate;
std::atomic<uint32_t> boot_presses{0}, boot_queued{0}, boot_handled{0}, start_stage{0};

void publish()
{
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    published = current;
    xSemaphoreGive(snapshot_lock);
}

void phase(VoicePhase value, const char *error = nullptr)
{
    if (current.phase != value) ++current.generation;
    current.phase = value;
    strlcpy(current.error, error ? error : "", sizeof(current.error));
    publish();
}

bool journal(bool save)
{
    nvs_handle_t handle;
    if (nvs_open("voice", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = save ? nvs_set_str(handle, "pending", request_id) : nvs_erase_key(handle, "pending");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool restore()
{
    nvs_handle_t handle;
    if (nvs_open("voice", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(request_id);
    bool ok = nvs_get_str(handle, "pending", request_id, &size) == ESP_OK && strlen(request_id) == 32;
    nvs_close(handle);
    if (!ok) request_id[0] = 0;
    return ok;
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
    if (!journal(false)) {
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
    current.duration_ms = current.peak = 0;
    phase(VoicePhase::Idle);
}

void begin()
{
    phase(VoicePhase::Preparing);
    start_stage = 1;
    release_audio();
    audio = static_cast<int16_t *>(heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    scratch = static_cast<int16_t *>(heap_caps_malloc(960 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!audio || !scratch) {
        release_audio();
        phase(VoicePhase::Unavailable, "audio_memory");
        return;
    }
    if (!codec_ready) {
        start_stage = 2;
        codec_ready = bsp_audio_init() == ESP_OK;
        if (codec_ready) codec_ready = esp_codec_dev_set_in_gain(bsp_audio_get_codec(), 36.f) == 0;
    }
    if (!codec_ready) {
        release_audio();
        phase(VoicePhase::Unavailable, "microphone_unavailable");
        return;
    }
    start_stage = 3;
    for (int i = 0; i < 32; ++i) {
        size_t discarded = 0;
        if (i2s_channel_read(bsp_audio_get_rx(), scratch, 960 * 4, &discarded, 0) != ESP_OK || !discarded) break;
    }
    for (int i = 0; i < 4; ++i) snprintf(request_id + i * 8, 9, "%08lx", (unsigned long)esp_random());
    recording_peak = 0;
    current.duration_ms = current.peak = 0;
    phase(VoicePhase::Recording);
    start_stage = 4;
}

void accept_reply(const VoiceReply &reply)
{
    if (!reply.ok) {
        phase(VoicePhase::UploadFailed, reply.error);
        return;
    }
    release_audio();
    current.page_count = reply.page_count;
    if (reply.phase == VoicePhase::Replied && !reply.read) {
        strlcpy(inbox_id, request_id, sizeof(inbox_id));
        current.unread = true;
    }
    phase(reply.phase, reply.error);
    next_poll = esp_timer_get_time() + 2000000;
}

void send()
{
    if (!voice_has_audio(samples, recording_peak)) {
        release_audio();
        phase(VoicePhase::Unavailable, "empty_recording");
        return;
    }
    if (!journal(true)) {
        phase(VoicePhase::Ready, "storage_unavailable");
        return;
    }
    phase(VoicePhase::Uploading);
    accept_reply(voice_exchange(VoiceRequest::Upload, request_id, audio, samples));
}

void load_page(uint16_t index)
{
    current.page_index = index;
    phase(VoicePhase::LoadingPage);
    uint8_t *downloaded = voice_download_page(request_id, index, current.page_count);
    if (!downloaded) { phase(VoicePhase::ReplyFailed, "page_download_failed"); return; }
    xSemaphoreTake(snapshot_lock, portMAX_DELAY);
    free(page_buffer);
    page_buffer = downloaded;
    ++current.generation;
    current.phase = VoicePhase::ViewingReply;
    current.error[0] = 0;
    published = current;
    xSemaphoreGive(snapshot_lock);
}

void open_reply()
{
    if (current.phase == VoicePhase::Idle || !current.page_count) {
        if (current.phase == VoicePhase::Idle) {
            if (!current.unread || !inbox_id[0]) return;
            strlcpy(request_id, inbox_id, sizeof(request_id));
        }
        phase(VoicePhase::LoadingPage);
        auto reply = voice_exchange(VoiceRequest::Status, request_id);
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
    phase(VoicePhase::MarkingRead);
    auto reply = voice_exchange(VoiceRequest::Read, request_id);
    if (!reply.ok || !reply.read) { phase(VoicePhase::ReplyFailed, "read_unconfirmed"); return; }
    current.unread = false;
    inbox_id[0] = 0;
    close();
}

void handle(const Input &input)
{
    if (input.action == VoiceAction::Boot && current.hold_id) return;
    if (!voice_input_current(input.action, input.origin, input.generation, current)) return;
    if (input.action == VoiceAction::Boot) ++boot_handled;
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
    case VoiceIntent::Send: send(); break;
    case VoiceIntent::RetryUpload:
        phase(VoicePhase::Uploading);
        accept_reply(audio ? voice_exchange(VoiceRequest::Upload, request_id, audio, samples) :
                            voice_exchange(VoiceRequest::Status, request_id));
        break;
    case VoiceIntent::RetryRemote:
        phase(VoicePhase::Uploading);
        accept_reply(voice_exchange(VoiceRequest::Retry, request_id));
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
    if (intent == VoiceHoldIntent::Send && samples >= VOICE_RATE * 3 / 10) send();
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

void worker(void *)
{
    bool recover = restore();
    if (recover) phase(VoicePhase::Uploading);
    for (;;) {
        xSemaphoreTake(snapshot_lock, portMAX_DELAY);
        const bool paused = settings_gate.boundary(current.phase,
            current.hold_id || hold_control.snapshot().id || uxQueueMessagesWaiting(inputs));
        xSemaphoreGive(snapshot_lock);
        if (paused) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        if (recover && begin_poll()) {
            accept_reply(voice_exchange(VoiceRequest::Status, request_id));
            finish_work();
            recover = false;
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
        if ((current.phase == VoicePhase::Stored || current.phase == VoicePhase::Transcribing ||
             current.phase == VoicePhase::Waiting || current.phase == VoicePhase::Claimed) &&
            esp_timer_get_time() >= next_poll && begin_poll()) {
            auto reply = voice_exchange(VoiceRequest::Status, request_id);
            if (reply.ok) accept_reply(reply);
            else phase(VoicePhase::UploadFailed, reply.error);
            next_poll = esp_timer_get_time() + 2000000;
            finish_work();
        }
        if (current.phase == VoicePhase::Idle && esp_timer_get_time() >= next_poll && begin_poll()) {
            auto reply = voice_exchange(VoiceRequest::Inbox, "");
            if (reply.ok) {
                bool unread = reply.phase == VoicePhase::Replied;
                if (current.unread != unread || strcmp(inbox_id, reply.request_id)) ++current.generation;
                current.unread = unread;
                strlcpy(inbox_id, reply.request_id, sizeof(inbox_id));
                publish();
            }
            next_poll = esp_timer_get_time() + 5000000;
            finish_work();
        }
    }
}

void button_worker(void *)
{
    VoiceButton button;
    for (;;) {
        if (button.update(gpio_get_level(BOARD_BOOT_IO) == 0, (uint32_t)(esp_timer_get_time() / 1000))) {
            ++boot_presses;
            auto snapshot = voice_snapshot();
            if (voice_action(VoiceAction::Boot, snapshot.generation)) ++boot_queued;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}

VoiceDiagnostics voice_diagnostics()
{
    return {gpio_get_level(BOARD_BOOT_IO) == 0, boot_presses.load(), boot_queued.load(),
            boot_handled.load(), start_stage.load()};
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
    hold_control.finish(id, outcome);
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
    if (xTaskCreate(worker, "voice", 12288, nullptr, 4, nullptr) != pdPASS) {
        phase(VoicePhase::Unavailable, "audio_memory");
        return;
    }
    if (xTaskCreate(button_worker, "voice_button", 3072, nullptr, 3, nullptr) != pdPASS)
        phase(VoicePhase::Unavailable, "button_unavailable");
}
