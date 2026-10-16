// Wi-Fi LAN TLS transport — see gadget_wifi.h. Design follows the reference
// SDK's GrokWifi (reference/grok-gadgets-esp32-sdk/lib/GrokGadgets/src):
// pinned-CA TLS to the gateway's LAN listener, credentials in NVS, and a
// periodic rejoin because the Wi-Fi core's own auto-reconnect gives up after
// a long access-point outage (SDK phase-4 router-off test).
#include "gadget_wifi.h"

#include <cstring>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "gadget_wifi";

static constexpr char NVS_NS[] = "grok";
static constexpr uint32_t JOIN_WAIT_MS = 10000;
static constexpr uint32_t MDNS_QUERY_MS = 1500;
static constexpr uint32_t TLS_HANDSHAKE_MS = 8000;

static EventGroupHandle_t s_wifi_eg;
static constexpr int WIFI_GOT_IP_BIT = BIT0;
static constexpr int WIFI_DOWN_BIT = BIT1;

static esp_tls_t *s_tls;
static volatile bool s_link_up;

/* ------------------------------------------------ NVS ---- */

static bool ensure_nvs()
{
    static bool done;
    if (done) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    done = err == ESP_OK;
    return done;
}

bool gadget_wifi_load(GadgetWifiCred &out)
{
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    GadgetWifiCred c = {};
    size_t n;
    bool ok = true;
    auto str = [&](const char *key, char *dst, size_t cap) {
        n = cap;
        if (nvs_get_str(h, key, dst, &n) != ESP_OK || n < 2) ok = false;
    };
    str("ssid", c.ssid, sizeof(c.ssid));
    n = sizeof(c.pass);  // open networks store an empty string: allowed
    if (nvs_get_str(h, "pass", c.pass, &n) != ESP_OK) ok = false;
    str("token", c.token, sizeof(c.token));
    str("host", c.host, sizeof(c.host));
    str("ca", c.ca, sizeof(c.ca));
    if (nvs_get_u16(h, "port", &c.port) != ESP_OK) ok = false;
    nvs_close(h);
    if (ok) out = c;
    return ok;
}

bool gadget_wifi_save(const GadgetWifiCred &cred)
{
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", cred.ssid) == ESP_OK &&
              nvs_set_str(h, "pass", cred.pass) == ESP_OK &&
              nvs_set_str(h, "token", cred.token) == ESP_OK &&
              nvs_set_str(h, "host", cred.host) == ESP_OK &&
              nvs_set_u16(h, "port", cred.port) == ESP_OK &&
              nvs_set_str(h, "ca", cred.ca) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool gadget_wifi_set_ssid(const char *ssid)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void gadget_wifi_erase()
{
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------ Wi-Fi bring-up ---- */

static void on_wifi(void *, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u)", (unsigned)event->reason);
        s_link_up = false;
        xEventGroupClearBits(s_wifi_eg, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_wifi_eg, WIFI_DOWN_BIT);
        // The core's auto-reconnect stops retrying after a long outage; the
        // transport loop reopens (and rejoins) on its own backoff instead.
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP acquired");
        xEventGroupClearBits(s_wifi_eg, WIFI_DOWN_BIT);
        xEventGroupSetBits(s_wifi_eg, WIFI_GOT_IP_BIT);
    }
}

static void wifi_ensure_init()
{
    static bool done;
    if (done) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Modem sleep keeps the ring on battery longer between beacons; the
    // protocol's 200 ms poll tolerates the wake latency.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    done = true;
}

static bool wifi_join(const GadgetWifiCred &cred)
{
    wifi_ensure_init();
    esp_wifi_stop();
    wifi_config_t sta = {};
    strlcpy((char *)sta.sta.ssid, cred.ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cred.pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = cred.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());   // STA_START handler connects
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_GOT_IP_BIT | WIFI_DOWN_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(JOIN_WAIT_MS));
    return bits & WIFI_GOT_IP_BIT;
}

/* ------------------------------------------------ failed-join diagnostics ---- */

static void diagnose_network_once(const char *ssid)
{
    static bool checked;
    if (checked) return;
    checked = true;
    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi diagnostic scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "Wi-Fi scan: no visible 2.4 GHz access points");
        return;
    }
    auto *aps = static_cast<wifi_ap_record_t *>(heap_caps_calloc(
        count, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return;
    }
    err = esp_wifi_scan_get_ap_records(&count, aps);
    if (err == ESP_OK) {
        unsigned matches = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!strcmp(reinterpret_cast<const char *>(aps[i].ssid), ssid)) {
                ++matches;
                ESP_LOGI(TAG, "target AP: channel=%u rssi=%d auth=%u",
                         aps[i].primary, aps[i].rssi, (unsigned)aps[i].authmode);
            }
        }
        ESP_LOGI(TAG, "Wi-Fi scan: visible=%u target_matches=%u", count, matches);
    } else {
        esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "Wi-Fi diagnostic results failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(aps);
}

/* ------------------------------------------------ host resolution ---- */

static bool resolve_host(const char *host, char *ip_out, size_t cap)
{
    size_t n = strlen(host);
    if (n > 6 && !strcmp(host + n - 6, ".local")) {
        // lwIP DNS does not do mDNS; ask the responder directly.
        char name[64];
        if (n - 6 >= sizeof(name)) return false;
        memcpy(name, host, n - 6);
        name[n - 6] = 0;
        static bool mdns_up;
        if (!mdns_up) {
            if (mdns_init() != ESP_OK) return false;
            mdns_up = true;
        }
        esp_ip4_addr_t addr;
        if (mdns_query_a(name, MDNS_QUERY_MS, &addr) != ESP_OK) return false;
        snprintf(ip_out, cap, IPSTR, IP2STR(&addr));
        return true;
    }
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != ESP_OK || !res) return false;
    auto *in = (struct sockaddr_in *)res->ai_addr;
    inet_ntoa_r(in->sin_addr, ip_out, cap);
    freeaddrinfo(res);
    return true;
}

/* ------------------------------------------------ TLS transport ---- */

bool gadget_wifi_transport_open(const GadgetWifiCred &cred)
{
    gadget_wifi_transport_close();
    if (!wifi_join(cred)) {
        ESP_LOGW(TAG, "wifi join failed");
        diagnose_network_once(cred.ssid);
        return false;
    }
    char ip[16];
    if (!resolve_host(cred.host, ip, sizeof(ip))) {
        ESP_LOGW(TAG, "resolve %s failed", cred.host);
        return false;
    }
    esp_tls_cfg_t cfg = {};
    cfg.cacert_buf = (const unsigned char *)cred.ca;
    cfg.cacert_bytes = strlen(cred.ca) + 1;
    cfg.timeout_ms = TLS_HANDSHAKE_MS;
    cfg.skip_common_name = true;  // the pinned CA is the trust decision; the
                                  // cert subject is the Mac's LAN address,
                                  // which DHCP can change under us
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return false;
    if (esp_tls_conn_new_sync(ip, strlen(ip), cred.port, &cfg, tls) < 0) {
        esp_tls_error_handle_t eh = nullptr;
        int code = 0;
        if (esp_tls_get_error_handle(tls, &eh) == ESP_OK && eh)
            esp_tls_get_and_clear_last_error(eh, &code, nullptr);
        ESP_LOGW(TAG, "tls connect failed: %d", code);
        esp_tls_conn_destroy(tls);
        return false;
    }
    // conn_read has no timeout parameter; SO_RCVTIMEO turns a blocking recv
    // into MBEDTLS_ERR_SSL_WANT_READ, which read() maps to "no data yet".
    int fd;
    if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK) {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s_tls = tls;
    s_link_up = true;
    ESP_LOGI(TAG, "lan tls up: %s:%u", ip, cred.port);
    return true;
}

void gadget_wifi_transport_close()
{
    if (s_tls) {
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
    }
    s_link_up = false;
}

bool gadget_wifi_transport_write(const char *buf, size_t len)
{
    if (!s_tls) return false;
    size_t off = 0;
    while (off < len) {
        int n = esp_tls_conn_write(s_tls, buf + off, len - off);
        if (n <= 0) { s_link_up = false; return false; }
        off += n;
    }
    return true;
}

int gadget_wifi_transport_read(char *buf, size_t cap, uint32_t timeout_ms)
{
    if (!s_tls || !s_link_up) return -1;
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    for (;;) {
        int n = esp_tls_conn_read(s_tls, buf, cap);
        if (n > 0) return n;
        if (n == 0 || n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if ((int64_t)(esp_timer_get_time() / 1000) >= deadline) return 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        s_link_up = false;   // peer closed or TLS alert: reopen the transport
        return -1;
    }
}

bool gadget_wifi_link_up()
{
    return s_link_up;
}
