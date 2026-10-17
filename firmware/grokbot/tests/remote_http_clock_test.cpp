#include "voice_http.h"
#include "voice_scope.h"
#include "voice_transport.h"
#include "esp_mac.h"
#include "gadget_clock.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
static time_t current_time, synced_time;
static bool configured = true, online = true, remote;
static int clock_starts, waits, clients, cleanups;
static std::string url, ca, authorization, device;
static esp_http_client_config_t last_config;
static int loads, opens, expected_body, written;
static bool rotated, rotate_after_load, max_credentials;
static std::string fail_header, content_type = "application/json";
static std::map<std::string, std::string> headers, sent_headers;
extern "C" time_t time(time_t *out) { if (out) *out = current_time; return current_time; }
int esp_netif_sntp_init(const esp_sntp_config_t *config)
{ assert(std::string(config->server) == "pool.ntp.org"); ++clock_starts; return 0; }
int esp_netif_sntp_sync_wait(int timeout)
{ assert(timeout == 10000); ++waits; current_time = synced_time; return 0; }
int esp_crt_bundle_attach(void *) { return 0; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *) { return online ? 0 : -1; }
esp_err_t esp_read_mac(uint8_t *mac, int) { memset(mac, 0xab, 6); return ESP_OK; }
bool gadget_wifi_load(GadgetWifiCred &c)
{
    ++loads;
    memset(&c,0,sizeof(c)); strcpy(c.host,"192.168.1.2"); c.port = 8767;
    strcpy(c.ca,"private-ca"); strcpy(c.token,"0123456789abcdef");
    if (remote) strcpy(c.remote_host,"public.example");
    if (rotated) strcpy(c.token, "rotated-enrollment-token");
    if (rotate_after_load) rotated = true;
    if (max_credentials) {
        memset(c.token, 't', sizeof(c.token) - 1);
        memset(c.remote_host, 'h', sizeof(c.remote_host) - 1);
    }
    return configured;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    ++clients; last_config = *config; url = config->url;
    ca = config->cert_pem ? config->cert_pem : "";
    headers.clear();
    return reinterpret_cast<void *>(1);
}
int esp_http_client_set_header(esp_http_client_handle_t, const char *key, const char *value)
{
    if (key == fail_header) return -1;
    headers[key] = value;
    if (!strcmp(key,"Authorization")) authorization = value;
    if (!strcmp(key,"X-Device-ID")) device = value;
    return 0;
}
int64_t esp_timer_get_time() { return 1000000; }
int esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *) { return ESP_OK; }
int esp_http_client_delete_header(esp_http_client_handle_t, const char *key) {
    return headers.erase(key) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
int esp_http_client_set_url(esp_http_client_handle_t, const char *value) { url = value; return ESP_OK; }
int esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t method) { last_config.method = method; return ESP_OK; }
int esp_http_client_cleanup(esp_http_client_handle_t) { ++cleanups; return 0; }
static void header(const char *value)
{
    esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, const_cast<char *>("content-type"),
                                const_cast<char *>(value), last_config.user_data};
    last_config.event_handler(&event);
}
int esp_http_client_open(esp_http_client_handle_t, int bytes)
{
    ++opens; expected_body = bytes; written = 0; sent_headers = headers;
    size_t wire_size = url.size() + 100;
    for (const auto &item : headers) wire_size += item.first.size() + item.second.size() + 4;
    assert(wire_size < size_t(last_config.buffer_size_tx));
    header(content_type.c_str());
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t, const char *, int size)
{ written += size; return size; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t)
{ assert(written == expected_body); return 2; }
int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
int esp_http_client_read_response(esp_http_client_handle_t, char *body, int size)
{ assert(size >= 2); memcpy(body, "{}", 2); return 2; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return true; }
// Protocol decoding is covered separately; this harness observes HTTP boundary behavior.
VoiceReply voice_decode(const char *body, const char *)
{ assert(!strcmp(body, "{}")); VoiceReply result; result.ok = true; return result; }

static void inbox_and_scope()
{
    online = true; remote = false;
    VoiceScope scope{};
    int before = loads;
    assert(voice_current_scope(scope) && loads == before + 1);
    before = loads;
    rotate_after_load = true;
    {
        VoiceHttp http("/v1/inbox", HTTP_METHOD_GET, 1500, 2048, &scope);
        assert(http.client && loads == before + 1 && last_config.buffer_size_tx == 2048);
        assert(headers["Authorization"].find("rotated") == std::string::npos);
    }
    rotate_after_load = false;
    before = clients;
    auto reply = voice_exchange(VoiceRequest::Read, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", nullptr, 0, &scope);
    assert(!reply.ok && !strcmp(reply.error, "scope_mismatch") && clients == before);
    for (auto request : {VoiceRequest::Upload, VoiceRequest::Status, VoiceRequest::Retry}) {
        reply = voice_exchange(request, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", nullptr, 0, &scope);
        assert(!reply.ok && !strcmp(reply.error, "scope_mismatch") && clients == before);
    }
    assert(!voice_download_page("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, 1, &scope));
    assert(clients == before);
    rotated = false; remote = true;
    assert(voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope).ok);
    assert(sent_headers.count("X-Voice-Read-Exclude") == 0);
    VoiceReadExclusions excluded;
    assert(voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope, &excluded).ok);
    assert(sent_headers.count("X-Voice-Read-Exclude") == 0);
    excluded.count = 16;
    std::string expected;
    for (size_t i = 0; i < excluded.count; ++i) {
        snprintf(excluded.ids[i], sizeof(excluded.ids[i]), "%032X", unsigned(i + 10));
        char lower[33]; snprintf(lower, sizeof(lower), "%032x", unsigned(i + 10));
        if (i) expected += ',';
        expected += lower;
    }
    max_credentials = true;
    assert(voice_current_scope(scope));
    assert(voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope, &excluded).ok);
    assert(sent_headers["X-Voice-Read-Exclude"] == expected && expected.size() == 527);
    assert(last_config.buffer_size_tx == 2048 && last_config.buffer_size == 1024);
    assert(last_config.method == HTTP_METHOD_GET && expected_body == 0);
    assert(url.substr(url.size() - 9) == "/v1/inbox" && headers["Authorization"].size() == 263);
    max_credentials = false;
    assert(voice_current_scope(scope));
    excluded.count = 1;
    assert(voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope, &excluded).ok);
    assert(sent_headers["X-Voice-Read-Exclude"] == expected.substr(0, 32));
    before = clients;
    for (int invalid : {0, 1, 2, 3}) {
        auto bad = excluded;
        if (invalid == 0) bad.count = 17;
        if (invalid == 1) bad.ids[0][4] = 'g';
        if (invalid == 2) bad.ids[0][31] = 0;
        if (invalid == 3) memset(bad.ids[0], 'a', sizeof(bad.ids[0]));
        reply = voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope, &bad);
        assert(!reply.ok && !strcmp(reply.error, "invalid_read_exclusions") && clients == before);
    }
    before = opens;
    for (const char *key : {"Authorization", "X-Device-ID", "Content-Type", "X-Voice-Read-Exclude"}) {
        fail_header = key;
        reply = voice_exchange(VoiceRequest::Inbox, "", nullptr, 0, &scope, &excluded);
        assert(!reply.ok && !strcmp(reply.error, "http_header_failed") && opens == before);
    }
    fail_header.clear();
    const char *id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (auto kind : {VoiceRequest::Upload, VoiceRequest::Status, VoiceRequest::Retry, VoiceRequest::Read}) {
        const int16_t samples[] = {12, 34};
        assert(voice_exchange(kind, id, samples, 2).ok); // original four-argument API
        assert(last_config.buffer_size_tx == 1024 && sent_headers.count("X-Voice-Read-Exclude") == 0);
        assert(last_config.method == (kind == VoiceRequest::Upload ? HTTP_METHOD_PUT :
            kind == VoiceRequest::Status ? HTTP_METHOD_GET : HTTP_METHOD_POST));
        const std::string suffix = kind == VoiceRequest::Read ? "/read" : kind == VoiceRequest::Retry ? "/retry" : "";
        assert(url == std::string("https://public.example:443/v1/recordings/") + id + suffix);
        assert(expected_body == (kind == VoiceRequest::Upload ? 48 : 0));
    }
    { VoiceHttp page("/v1/recordings/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/pages/0", HTTP_METHOD_GET);
      assert(page.client && last_config.buffer_size_tx == 1024); }
    content_type = "text/html";
    reply = voice_exchange(VoiceRequest::Inbox, "");
    assert(!reply.ok && !strcmp(reply.error, "http_unexpected_response"));
    assert(clients == cleanups);
    puts("Inbox 0/1/16 exclusions, 527-byte complete header, 17/invalid rejection, scope same snapshot, header failures and unchanged routes PASS");
}
int main()
{
    assert(!gadget_clock_ready()); assert(!gadget_clock_wait());
    assert(clock_starts == 1 && waits == 1);
    { VoiceHttp http("/v1/inbox",HTTP_METHOD_GET,1500); assert(!http.client); assert(!strcmp(http.error,"time_unavailable")); }
    assert(clients == 0);
    synced_time = 1792200000; assert(gadget_clock_wait()); assert(gadget_clock_ready());
    assert(clock_starts == 1 && waits == 2);
    assert(gadget_clock_wait()); assert(waits == 2); // warm clock survives loss of SNTP
    {
        VoiceHttp http("/v1/inbox",HTTP_METHOD_GET,1500); assert(http.client);
        assert(url == "https://192.168.1.2:8768/v1/inbox" && ca == "private-ca");
        assert(last_config.skip_cert_common_name_check && !last_config.crt_bundle_attach);
        assert(last_config.timeout_ms == 1500 && last_config.disable_auto_redirect);
    }
    remote = true;
    {
        VoiceHttp http("/v1/inbox",HTTP_METHOD_GET,1500); assert(http.client);
        assert(url == "https://public.example:443/v1/inbox" && ca.empty());
        assert(!last_config.skip_cert_common_name_check && last_config.crt_bundle_attach);
        assert(last_config.timeout_ms == 15000 && last_config.disable_auto_redirect);
        assert(authorization == "Bearer 0123456789abcdef" && device == "smartring-abababababab");
        assert(!http.json_response); header("text/html"); assert(!http.json_response);
        header("application/json; charset=utf-8"); assert(http.json_response);
        header("application/json-injected"); assert(!http.json_response);
    }
    online = false;
    { VoiceHttp http("/v1/inbox",HTTP_METHOD_GET); assert(!http.client); assert(!strcmp(http.error,"wifi_offline")); }
    assert(clients == 2 && cleanups == 2);
    inbox_and_scope();
    current_time = 4102444800LL; assert(!gadget_clock_ready());
    puts("Shared clock gate, bounded SNTP, warm LAN fallback, public HTTPS443 trust/timeout, redirect and HTML rejection PASS");
}
