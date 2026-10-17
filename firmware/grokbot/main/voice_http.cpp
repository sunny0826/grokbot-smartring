#include "voice_http.h"
#include "voice_scope.h"
#include "gadget_clock.h"
#include "gadget_endpoint.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include <mutex>
#include <new>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
std::mutex session_mutex;
VoiceHttp *cached = nullptr;
uint32_t session_epoch = 0;
bool session_busy = false;
bool watching_wifi = false;
void disconnected(void *, esp_event_base_t, int32_t, void *) { voice_http_invalidate(); }
bool remove_header(esp_http_client_handle_t client, const char *key) {
    const auto result = esp_http_client_delete_header(client, key);
    return result == ESP_OK || result == ESP_ERR_NOT_FOUND;
}
bool media_type(const char *value, const char *type) {
    size_t n = strlen(type);
    return !strncasecmp(value, type, n) && (value[n] == 0 || value[n] == ';' || value[n] == ' ');
}
}

esp_err_t VoiceHttp::on_event(esp_http_client_event_t *event)
{
    auto *http = static_cast<VoiceHttp *>(event->user_data);
    if (event->event_id == HTTP_EVENT_DISCONNECTED) http->keep_alive = false;
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    const char *key = event->header_key, *value = event->header_value;
    if (!strcasecmp(key, "Content-Type")) {
        http->metadata_valid &= ++http->content_type_headers == 1;
        http->json_response = media_type(value, "application/json");
        http->binary_response = media_type(value, "application/octet-stream");
    } else if (!strcasecmp(key, "Connection")) {
        http->metadata_valid &= ++http->connection_headers == 1;
        http->keep_alive = !strcasecmp(value, "keep-alive");
    } else if (!strcasecmp(key, "X-Voice-Request-ID")) {
        ++http->request_id_headers;
        if (strlen(value) != 32) http->metadata_valid = false;
        else memcpy(http->response_id, value, 33);
    } else if (!strcasecmp(key, "X-Voice-Read")) {
        ++http->read_headers;
        if (strcmp(value, "0") && strcmp(value, "1")) http->metadata_valid = false;
        http->response_read = !strcmp(value, "1");
    }
    return ESP_OK;
}

VoiceHttp::VoiceHttp(const char *path, esp_http_client_method_t method, int timeout_ms,
                     int tx_buffer_size, const VoiceScope *expected_scope)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { error = "wifi_offline"; return; }
    if (!gadget_clock_ready()) { error = "time_unavailable"; return; }
    credentials = static_cast<GadgetWifiCred *>(heap_caps_calloc(1, sizeof(GadgetWifiCred), MALLOC_CAP_SPIRAM));
    if (!credentials) { error = "audio_memory"; return; }
    if (!gadget_wifi_load(*credentials) || (!credentials->remote_host[0] && credentials->port == 65535)) {
        error = "wifi_not_configured"; return;
    }
    if (expected_scope && !voice_scope_matches(*expected_scope, *credentials)) {
        error = "scope_mismatch"; return;
    }
    configured = true;
    initialize(path, method, timeout_ms, tx_buffer_size);
}

void VoiceHttp::initialize(const char *path, esp_http_client_method_t method,
                           int timeout_ms, int tx_buffer_size)
{
    const auto endpoint = gadget_endpoint(*credentials);
    char url[256], authorization[272], device[24];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device, sizeof(device), "smartring-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(url, sizeof(url), "https://%s:%u%s", endpoint.host,
             endpoint.voice_port, path);
    snprintf(authorization, sizeof(authorization), "Bearer %s", credentials->token);
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = on_event;
    config.user_data = this;
    config.cert_pem = endpoint.remote ? nullptr : credentials->ca;
    config.crt_bundle_attach = endpoint.remote ? esp_crt_bundle_attach : nullptr;
    config.skip_cert_common_name_check = !endpoint.remote;
    config.timeout_ms = endpoint.remote && timeout_ms < 15000 ? 15000 : timeout_ms;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.buffer_size = 1024;
    config.buffer_size_tx = tx_buffer_size;
    config.method = method;
    client = esp_http_client_init(&config);
    if (client) {
        if (esp_http_client_set_header(client, "Authorization", authorization) != ESP_OK ||
            esp_http_client_set_header(client, "X-Device-ID", device) != ESP_OK) {
            error = "http_header_failed";
            esp_http_client_cleanup(client);
            client = nullptr;
        }
    }
    memset(authorization, 0, sizeof(authorization));
    created_us = last_us = esp_timer_get_time();
    requests = 0;
}

VoiceHttp::~VoiceHttp()
{
    disconnect();
    if (credentials) { memset(credentials, 0, sizeof(*credentials)); free(credentials); }
}

void VoiceHttp::disconnect()
{
    if (client) esp_http_client_cleanup(client);
    client = nullptr;
    keep_alive = false;
}

bool VoiceHttp::matches(const VoiceScope *scope) const
{
    wifi_ap_record_t ap{};
    if (!configured || !credentials || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || !gadget_clock_ready()) return false;
    auto *current = static_cast<GadgetWifiCred *>(heap_caps_calloc(1, sizeof(GadgetWifiCred), MALLOC_CAP_SPIRAM));
    if (!current) return false;
    const bool same = gadget_wifi_load(*current) && !memcmp(current, credentials, sizeof(*current)) &&
        (!scope || voice_scope_matches(*scope, *current));
    memset(current, 0, sizeof(*current));
    free(current);
    return same;
}

bool VoiceHttp::prepare(const char *path)
{
    const int64_t now = esp_timer_get_time();
    if (now - last_us >= 2500000 || now - created_us >= 28000000 || requests >= 64) disconnect();
    reused_connection = client && keep_alive;
    json_response = binary_response = keep_alive = false;
    metadata_valid = true;
    request_id_headers = read_headers = content_type_headers = connection_headers = 0;
    response_id[0] = 0;
    response_read = false;
    if (!configured) return false;
    if (!client && credentials) initialize(path, HTTP_METHOD_GET, 8000, 1024);
    if (!client) return false;
    const auto endpoint = gadget_endpoint(*credentials);
    char url[256];
    snprintf(url, sizeof(url), "https://%s:%u%s", endpoint.host, endpoint.voice_port, path);
    ++requests;
    last_us = now;
    return esp_http_client_set_url(client, url) == ESP_OK &&
        esp_http_client_set_method(client, HTTP_METHOD_GET) == ESP_OK &&
        remove_header(client, "Content-Type") &&
        remove_header(client, "X-Voice-After") &&
        remove_header(client, "X-Voice-Read-Exclude") &&
        esp_http_client_set_header(client, "Connection", "keep-alive") == ESP_OK;
}

void voice_http_invalidate()
{
    VoiceHttp *old;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        ++session_epoch;
        old = cached;
        cached = nullptr;
    }
    delete old;
}

VoiceHttpSession::VoiceHttpSession(const char *path, const VoiceScope *scope)
{
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (session_busy) return;
        if (!watching_wifi) {
            if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                           disconnected, nullptr) != ESP_OK) return;
            watching_wifi = true;
        }
        session_busy = acquired = true;
        epoch = session_epoch;
        http = cached;
        cached = nullptr;
    }
    if (http && !http->matches(scope)) { delete http; http = nullptr; }
    if (!http) http = new (std::nothrow) VoiceHttp(path, HTTP_METHOD_GET, 8000, 1024, scope);
    if (http && !http->prepare(path)) http->disconnect();
}

VoiceHttpSession::~VoiceHttpSession()
{
    if (!acquired) return;
    if (http && (!accepted || !http->metadata_valid || !http->keep_alive)) http->disconnect();
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (epoch == session_epoch) { cached = http; http = nullptr; }
        session_busy = false;
    }
    delete http;
}
