#include "voice_transport.h"
#include "voice_logic.h"
#include "voice_http.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
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
                         const int16_t *samples, size_t count)
{
    VoiceReply result;
    strlcpy(result.error, "upload_unconfirmed", sizeof(result.error));
    char path[128];
    if (request == VoiceRequest::Inbox) strlcpy(path, "/v1/inbox", sizeof(path));
    else snprintf(path, sizeof(path), "/v1/recordings/%s%s", request_id,
                  request == VoiceRequest::Retry ? "/retry" : request == VoiceRequest::Read ? "/read" : "");
    VoiceHttp http(path, request == VoiceRequest::Upload ? HTTP_METHOD_PUT :
        request == VoiceRequest::Retry || request == VoiceRequest::Read ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        request == VoiceRequest::Inbox ? 1500 : 8000);
    auto client = http.client;
    if (!client) strlcpy(result.error, http.error, sizeof(result.error));
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "audio/wav");
        uint8_t header[44];
        voice_wav_header(header, count);
        size_t bytes = request == VoiceRequest::Upload ? 44 + count * 2 : 0;
        bool ok = esp_http_client_open(client, bytes) == ESP_OK;
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
    return result;
}
