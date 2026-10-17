#include "voice_http.h"
#include "voice_scope.h"
#include "gadget_clock.h"
#include "gadget_endpoint.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include <cstdio>
#include <cstring>
#include <strings.h>

esp_err_t VoiceHttp::on_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        !strcasecmp(event->header_key, "Content-Type")) {
        auto *http = static_cast<VoiceHttp *>(event->user_data);
        const char *value = event->header_value;
        http->json_response = !strncasecmp(value, "application/json", 16) &&
            (value[16] == 0 || value[16] == ';' || value[16] == ' ');
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
}

VoiceHttp::~VoiceHttp()
{
    if (client) esp_http_client_cleanup(client);
    if (credentials) { memset(credentials, 0, sizeof(*credentials)); free(credentials); }
}
