#include "voice_transport.h"
#include "voice_http.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdlib>

uint8_t *voice_download_page(const char *request_id, uint16_t index, uint16_t count)
{
    if (!count || count > VOICE_PAGE_LIMIT || index >= count) return nullptr;
    char path[128];
    snprintf(path, sizeof(path), "/v1/recordings/%s/pages/%u", request_id, index);
    VoiceHttp http(path, HTTP_METHOD_GET);
    auto client = http.client;
    constexpr size_t size = VOICE_PAGE_HEADER + VOICE_PAGE_BYTES;
    if (!client || esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) != size || esp_http_client_get_status_code(client) != 200)
        return nullptr;
    auto *page = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!page) return nullptr;
    int n = esp_http_client_read_response(client, reinterpret_cast<char *>(page), size);
    if (n != size || !esp_http_client_is_complete_data_received(client) ||
        !voice_page_valid(page, size, index, count)) { free(page); return nullptr; }
    return page;
}
