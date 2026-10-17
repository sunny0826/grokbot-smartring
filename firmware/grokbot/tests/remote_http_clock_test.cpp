#include "voice_http.h"
#include "gadget_clock.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
static time_t current_time, synced_time;
static bool configured = true, online = true, remote;
static int clock_starts, waits, clients, cleanups;
static std::string url, ca, authorization, device;
static esp_http_client_config_t last_config;
extern "C" time_t time(time_t *out) { if (out) *out = current_time; return current_time; }
int esp_netif_sntp_init(const esp_sntp_config_t *config)
{ assert(std::string(config->server) == "pool.ntp.org"); ++clock_starts; return 0; }
int esp_netif_sntp_sync_wait(int timeout)
{ assert(timeout == 10000); ++waits; current_time = synced_time; return 0; }
int esp_crt_bundle_attach(void *) { return 0; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *) { return online ? 0 : -1; }
void esp_read_mac(uint8_t *mac, int) { memset(mac, 0xab, 6); }
bool gadget_wifi_load(GadgetWifiCred &c)
{
    memset(&c,0,sizeof(c)); strcpy(c.host,"192.168.1.2"); c.port = 8767;
    strcpy(c.ca,"private-ca"); strcpy(c.token,"0123456789abcdef");
    if (remote) strcpy(c.remote_host,"public.example"); return configured;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    ++clients; last_config = *config; url = config->url;
    ca = config->cert_pem ? config->cert_pem : "";
    return reinterpret_cast<void *>(1);
}
int esp_http_client_set_header(esp_http_client_handle_t, const char *key, const char *value)
{
    if (!strcmp(key,"Authorization")) authorization = value;
    if (!strcmp(key,"X-Device-ID")) device = value;
    return 0;
}
int esp_http_client_cleanup(esp_http_client_handle_t) { ++cleanups; return 0; }
static void header(const char *value)
{
    esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, const_cast<char *>("content-type"),
                                const_cast<char *>(value), last_config.user_data};
    last_config.event_handler(&event);
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
    current_time = 4102444800LL; assert(!gadget_clock_ready());
    puts("Shared clock gate, bounded SNTP, warm LAN fallback, public HTTPS443 trust/timeout, redirect and HTML rejection PASS");
}
