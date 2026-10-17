// Wi-Fi LAN TLS transport — see gadget_wifi.h. Design follows the reference
// SDK's GrokWifi (reference/grok-gadgets-esp32-sdk/lib/GrokGadgets/src):
// pinned-CA TLS to the gateway's LAN listener, credentials in NVS, and a
// periodic rejoin because the Wi-Fi core's own auto-reconnect gives up after
// a long access-point outage (SDK phase-4 router-off test).
#include "gadget_endpoint.h"
#include "gadget_clock.h"
#include "gadget_ws.h"
#include "wifi_setup_owner.h"
#include "wifi_setup_driver.h"
#include <atomic>

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
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "gadget_wifi";

static constexpr uint32_t JOIN_WAIT_MS = 10000;
static constexpr uint32_t MDNS_QUERY_MS = 1500;
static constexpr uint32_t TLS_HANDSHAKE_MS = 8000;

static esp_tls_t *s_tls;
static std::atomic<bool> s_link_up;
static bool s_remote;
static std::atomic<const char *> s_error{""};
const char *gadget_wifi_transport_error() { return s_error.load(); }
void gadget_wifi_set_transport_error(const char *error) { s_error.store(error); }

/* ------------------------------------------------ Wi-Fi bring-up ---- */

static bool wifi_join(const GadgetWifiCred &cred)
{
    WifiSetupNetwork network{};
    memcpy(network.ssid, cred.ssid, sizeof(network.ssid));
    memcpy(network.password, cred.pass, sizeof(network.password));
    // Poll the owner adapter to consume disconnect events before the fast path.
    wifi_driver_poll();
    if (wifi_driver_connected(network)) return true;
    if (wifi_setup_requested() || !wifi_driver_join(network)) return false;
    int64_t deadline = wifi_setup_now_ms() + JOIN_WAIT_MS;
    while (!wifi_setup_requested() && wifi_setup_now_ms() < deadline) {
        auto result = wifi_driver_poll();
        if (result == WifiDriverResult::Connected) return wifi_driver_connected(network);
        if (result != WifiDriverResult::Pending) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
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
        s_error = "wifi_offline";
        ESP_LOGW(TAG, "wifi join failed");
        return false;
    }
    if (wifi_setup_requested()) return false;
    if (!gadget_clock_ready()) s_error = "time_unavailable";
    if (!gadget_clock_wait()) { s_error = "time_unavailable"; return false; }
    if (wifi_setup_requested()) return false;
    const auto endpoint = gadget_endpoint(cred);
    s_remote = endpoint.remote;
    if (s_remote) {
        s_link_up = gadget_ws_open(cred);
        if (wifi_setup_requested()) { gadget_wifi_transport_close(); return false; }
        s_error = s_link_up ? "" : "wss_connect_failed";
        return s_link_up;
    }
    char ip[16];
    if (!resolve_host(cred.host, ip, sizeof(ip))) {
        s_error = "dns_failed";
        ESP_LOGW(TAG, "resolve %s failed", cred.host);
        return false;
    }
    if (wifi_setup_requested()) return false;
    esp_tls_cfg_t cfg = {};
    cfg.cacert_buf = (const unsigned char *)cred.ca;
    cfg.cacert_bytes = strlen(cred.ca) + 1;
    cfg.timeout_ms = TLS_HANDSHAKE_MS;
    cfg.skip_common_name = true;  // the pinned CA is the trust decision; the
                                  // cert subject is the Mac's LAN address,
                                  // which DHCP can change under us
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return false;
    if (esp_tls_conn_new_sync(ip, strlen(ip), cred.port, &cfg, tls) != 1) {
        esp_tls_error_handle_t eh = nullptr;
        int code = 0;
        if (esp_tls_get_error_handle(tls, &eh) == ESP_OK && eh)
            esp_tls_get_and_clear_last_error(eh, &code, nullptr);
        s_error = "tls_connect_failed";
        ESP_LOGW(TAG, "tls connect failed: %d", code);
        esp_tls_conn_destroy(tls);
        return false;
    }
    if (wifi_setup_requested()) { esp_tls_conn_destroy(tls); return false; }
    // conn_read has no timeout parameter; SO_RCVTIMEO turns a blocking recv
    // into MBEDTLS_ERR_SSL_WANT_READ, which read() maps to "no data yet".
    int fd;
    if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK) {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s_error = "";
    s_tls = tls;
    s_link_up = true;
    ESP_LOGI(TAG, "lan tls up: %s:%u", ip, cred.port);
    return true;
}

void gadget_wifi_transport_close()
{
    gadget_ws_close();
    if (s_tls) {
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
    }
    s_link_up = false;
}

bool gadget_wifi_transport_write(const char *buf, size_t len)
{
    if (!s_link_up) return false;
    if (s_remote) {
        bool ok = gadget_ws_write(buf, len);
        if (!ok) { s_link_up = false; s_error = "write_failed"; }
        return ok;
    }
    if (!s_tls) return false;
    size_t off = 0;
    while (off < len) {
        int n = esp_tls_conn_write(s_tls, buf + off, len - off);
        if (n <= 0) { s_link_up = false; s_error = "write_failed"; return false; }
        off += n;
    }
    return true;
}

int gadget_wifi_transport_read(char *buf, size_t cap, uint32_t timeout_ms)
{
    if (!s_link_up) return -1;
    if (s_remote) {
        int n = gadget_ws_read(buf, cap, timeout_ms);
        if (n < 0) { s_link_up = false; s_error = "wss_read_failed"; }
        return n;
    }
    if (!s_tls) return -1;
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    for (;;) {
        int n = esp_tls_conn_read(s_tls, buf, cap);
        if (n > 0) return n;
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if ((int64_t)(esp_timer_get_time() / 1000) >= deadline) return 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        s_error = "tls_read_failed";
        s_link_up = false;   // peer closed or TLS alert: reopen the transport
        return -1;
    }
}

bool gadget_wifi_link_up()
{
    return s_link_up;
}
