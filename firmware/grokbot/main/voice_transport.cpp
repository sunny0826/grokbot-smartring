#include "voice_transport.h"
#include "voice_logic.h"
#include "voice_http.h"
#include "voice_timing.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
bool exclusion_header(const VoiceReadExclusions &excluded, char (&value)[528])
{
    if (excluded.count > VOICE_READ_LIMIT) return false;
    size_t offset = 0;
    for (size_t i = 0; i < excluded.count; ++i) {
        const char *id = excluded.ids[i];
        if (strnlen(id, sizeof(excluded.ids[i])) != 32) return false;
        if (i) value[offset++] = ',';
        for (size_t j = 0; j < 32; ++j) {
            const char c = id[j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
            value[offset++] = c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c;
        }
    }
    value[offset] = 0;
    return true;
}

bool write_all(esp_http_client_handle_t client, const void *buffer, size_t size)
{
    auto bytes = static_cast<const char *>(buffer);
    while (size) {
        int n = esp_http_client_write(client, bytes, (int)std::min(size, size_t(4096)));
        if (n <= 0) return false;
        bytes += n;
        size -= n;
    }
    return true;
}


}

VoiceReply voice_exchange(VoiceRequest request, const char *request_id,
                         const int16_t *samples, size_t count,
                         const VoiceScope *scope, const VoiceReadExclusions *excluded)
{
    if (request == VoiceRequest::Response) {
        VoiceResponse response = voice_response(request_id, VoicePhase::Idle, scope);
        free(response.page);
        return response.reply;
    }
    VoiceReply result;
    strlcpy(result.error, "upload_unconfirmed", sizeof(result.error));
    char exclusions[528] = {};
    if (request == VoiceRequest::Inbox && excluded && !exclusion_header(*excluded, exclusions)) {
        strlcpy(result.error, "invalid_read_exclusions", sizeof(result.error));
        return result;
    }
    char path[128];
    if (request == VoiceRequest::Inbox) strlcpy(path, "/v1/inbox", sizeof(path));
    else snprintf(path, sizeof(path), "/v1/recordings/%s%s", request_id,
                  request == VoiceRequest::Retry ? "/retry" : request == VoiceRequest::Read ? "/read" : "");
    VoiceHttp http(path, request == VoiceRequest::Upload ? HTTP_METHOD_PUT :
        request == VoiceRequest::Retry || request == VoiceRequest::Read ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        request == VoiceRequest::Inbox ? 1500 : 8000,
        request == VoiceRequest::Inbox ? 2048 : 1024, scope);
    auto client = http.client;
    if (!client) strlcpy(result.error, http.error, sizeof(result.error));
    if (client) {
        if (esp_http_client_set_header(client, "Content-Type", "audio/wav") != ESP_OK ||
            (exclusions[0] && esp_http_client_set_header(client, "X-Voice-Read-Exclude", exclusions) != ESP_OK)) {
            strlcpy(result.error, "http_header_failed", sizeof(result.error));
            return result;
        }
        uint8_t header[44];
        voice_wav_header(header, count);
        size_t bytes = request == VoiceRequest::Upload ? 44 + count * 2 : 0;
        bool ok = esp_http_client_open(client, bytes) == ESP_OK;
        if (ok && request == VoiceRequest::Upload) voice_timing_mark(request_id, VoiceTimingStage::Connected);
        if (ok && bytes) ok = write_all(client, header, sizeof(header)) && write_all(client, samples, count * 2);
        if (ok) ok = esp_http_client_fetch_headers(client) >= 0;
        if (ok) {
            int status = esp_http_client_get_status_code(client);
            char body[1024];
            int n = esp_http_client_read_response(client, body, sizeof(body) - 1);
            if (status >= 200 && status < 300 && http.json_response && n > 0 && n < (int)sizeof(body) - 1 &&
                esp_http_client_is_complete_data_received(client)) {
                body[n] = 0;
                result = voice_decode(body, request == VoiceRequest::Inbox ? nullptr : request_id);
            } else {
                const char *error = status >= 200 && status < 300 && !http.json_response ? "http_unexpected_response" :
                    status == 401 || status == 403 ? "device_unauthorized" :
                    status == 404 ? "recording_not_found" : status == 409 ? "recording_conflict" :
                    status == 413 ? "recording_too_large" : status == 507 ? "storage_full" : "upload_unconfirmed";
                strlcpy(result.error, error, sizeof(result.error));
            }
        }
    }
    if (request == VoiceRequest::Upload && result.ok)
        voice_timing_mark(request_id, VoiceTimingStage::UploadConfirmed);
    return result;
}
