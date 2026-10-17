#include "voice_transport.h"
#include "voice_http.h"
#include "voice_timing.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const char *after_header(VoicePhase phase)
{
    switch (phase) {
    case VoicePhase::Stored: return "stored";
    case VoicePhase::Transcribing: return "transcribing";
    case VoicePhase::Waiting: return "ready";
    case VoicePhase::Claimed: return "claimed";
    default: return nullptr;
    }
}

void first_page(VoiceResponse &result, const char *request_id, const VoiceScope *scope)
{
    if (!result.reply.ok || result.reply.phase != VoicePhase::Replied) return;
    voice_timing_mark(request_id, VoiceTimingStage::ReplyReady);
    if (!result.page) result.page = voice_download_page(request_id, 0, result.reply.page_count, scope);
    if (!result.page) {
        result.reply.ok = false;
        strlcpy(result.reply.error, "page_download_failed", sizeof(result.reply.error));
        return;
    }
    voice_timing_mark(request_id, VoiceTimingStage::PageValidated);
}

VoiceResponse legacy(const char *request_id, const VoiceScope *scope)
{
    VoiceResponse result;
    result.reply = voice_exchange(VoiceRequest::Status, request_id, nullptr, 0, scope);
    first_page(result, request_id, scope);
    return result;
}
}

static VoiceResponse response_once(const char *request_id, VoicePhase after, const VoiceScope *scope, bool &reconnect)
{
    VoiceResponse result;
    strlcpy(result.reply.error, "invalid_response", sizeof(result.reply.error));
    if (!request_id || strlen(request_id) != 32 || strspn(request_id, "0123456789" "abcdef") != 32) return result;
    char path[128];
    snprintf(path, sizeof(path), "/v1/recordings/%s/response", request_id);
    VoiceHttpSession session(path, scope);
    auto *http = session.http;
    if (!http || !http->client) {
        strlcpy(result.reply.error, http ? http->error : "connection_failed", sizeof(result.reply.error));
        return result;
    }
    if (http->legacy_response) { http->disconnect(); return legacy(request_id, scope); }
    auto client = http->client;
    auto interrupted = [&] { reconnect = http->reused_connection; return result; };
    const char *after_value = after_header(after);
    if (after_value && esp_http_client_set_header(client, "X-Voice-After", after_value) != ESP_OK) return result;
    if (esp_http_client_open(client, 0) != ESP_OK) return interrupted();
    const int64_t length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (length < 0) return interrupted();
    if (status == 404 || status == 405 || status == 501) {
        http->legacy_response = true;
        http->disconnect();
        return legacy(request_id, scope);
    }
    if (status != 200 || !http->metadata_valid) {
        if (status == 401 || status == 403)
            strlcpy(result.reply.error, "device_unauthorized", sizeof(result.reply.error));
        return result;
    }
    if (http->json_response) {
        char body[1024];
        if (length >= int64_t(sizeof(body) - 1)) return result;
        const int n = esp_http_client_read_response(client, body, sizeof(body) - 1);
        if (n >= int(sizeof(body) - 1)) return result;
        if (!esp_http_client_is_complete_data_received(client)) return interrupted();
        if (n <= 0) return result;
        body[n] = 0;
        result.reply = voice_decode(body, request_id);
        if (result.reply.ok && result.reply.phase == VoicePhase::Replied) http->disconnect();
        first_page(result, request_id, scope);
        session.accepted = result.reply.ok && (result.reply.phase == VoicePhase::Stored ||
            result.reply.phase == VoicePhase::Transcribing || result.reply.phase == VoicePhase::Waiting ||
            result.reply.phase == VoicePhase::Claimed);
        return result;
    }
    constexpr size_t size = VOICE_PAGE_HEADER + VOICE_PAGE_BYTES;
    if (!http->binary_response || length != size || http->request_id_headers != 1 ||
        http->read_headers != 1 || strcmp(http->response_id, request_id)) return result;
    voice_timing_mark(request_id, VoiceTimingStage::ReplyReady);
    auto *page = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!page) return result;
    const int n = esp_http_client_read_response(client, reinterpret_cast<char *>(page), size);
    const uint16_t count = n == size ? voice_page_le(page + 10, 2) : 0;
    if (!esp_http_client_is_complete_data_received(client)) { free(page); return interrupted(); }
    if (n != size || !voice_page_valid(page, size, 0, count)) { free(page); return result; }
    result.page = page;
    result.reply.ok = true;
    result.reply.phase = VoicePhase::Replied;
    result.reply.page_count = count;
    result.reply.read = http->response_read;
    strlcpy(result.reply.request_id, request_id, sizeof(result.reply.request_id));
    result.reply.error[0] = 0;
    voice_timing_mark(request_id, VoiceTimingStage::PageValidated);
    return result;
}

VoiceResponse voice_response(const char *request_id, VoicePhase after, const VoiceScope *scope)
{
    bool reconnect = false;
    auto result = response_once(request_id, after, scope, reconnect);
    if (!reconnect) return result;
    voice_http_invalidate();
    return response_once(request_id, after, scope, reconnect);
}
