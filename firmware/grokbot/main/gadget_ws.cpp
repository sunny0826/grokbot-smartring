#include "gadget_ws.h"
#include "gadget_endpoint.h"
#include "gadget_ws_message.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#if !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
#error "WSS requires per-tag log control to keep Upgrade credentials out of logs"
#endif
#include <cstdio>
#include <cstring>

static esp_transport_handle_t s_ssl, s_ws;
static GadgetWsMessage s_message;
static size_t s_delivered;
static bool s_ready;

void gadget_ws_close()
{
    if (s_ws) {
        esp_transport_close(s_ws); // closes parent socket, not parent allocation
        esp_transport_destroy(s_ws);
        s_ws = nullptr;
    }
    if (s_ssl) { esp_transport_destroy(s_ssl); s_ssl = nullptr; }
    s_message.reset();
    s_ready = false;
    s_delivered = 0;
}

bool gadget_ws_open(const GadgetWifiCred &cred)
{
    gadget_ws_close();
    // IDF 5.5.3 logs the full Upgrade buffer (including Bearer) on write failure.
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    if (!cred.remote_host[0] || !gadget_remote_host_valid(cred.remote_host) ||
        !gadget_header_value_valid(cred.token)) return false;
    s_ssl = esp_transport_ssl_init();
    if (!s_ssl) return false;
    esp_transport_ssl_crt_bundle_attach(s_ssl, esp_crt_bundle_attach);
    s_ws = esp_transport_ws_init(s_ssl);
    if (!s_ws) { gadget_ws_close(); return false; }
    char auth[272], headers[64];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(auth, sizeof(auth), "Bearer %s", cred.token);
    snprintf(headers, sizeof(headers), "X-Device-ID: smartring-%02x%02x%02x%02x%02x%02x\r\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_transport_ws_config_t config = {};
    config.ws_path = "/v1/control";
    config.auth = auth;
    config.headers = headers;
    config.propagate_control_frames = true;
    esp_err_t configured = esp_transport_ws_set_config(s_ws, &config);
    memset(auth, 0, sizeof(auth));
    // Pass the DNS name through to ESP-TLS for SNI and certificate matching.
    if (configured != ESP_OK || esp_transport_connect(s_ws, cred.remote_host, 443, 8000) != 0 ||
        esp_transport_ws_get_upgrade_request_status(s_ws) != 101) {
        gadget_ws_close();
        return false;
    }
    return true;
}

bool gadget_ws_write(const char *data, size_t size)
{
    if (!s_ws || size < 2 || size > 2208 || data[size - 1] != '\n' ||
        memchr(data, '\n', size - 1)) return false;
    // IDF masks in-place, including a header-failure path that leaves it masked.
    char writable[2208];
    memcpy(writable, data, size);
    int written = esp_transport_write(s_ws, writable, size, 4000);
    if (written == static_cast<int>(size)) return true;
    // A second write would be a second WS message, not the missing suffix.
    gadget_ws_close();
    return false;
}

int gadget_ws_read(char *data, size_t capacity, uint32_t timeout_ms)
{
    if (!s_ws || !capacity) return -1;
    if (!s_ready) {
        int available = esp_transport_poll_read(s_ws, timeout_ms);
        if (available < 0) { gadget_ws_close(); return -1; }
        if (!available) return 0;
        char chunk[256];
        // The IDF parser drops a partially read payload on timeout. Give an
        // available frame a bounded read window and close if it times out.
        int n = esp_transport_read(s_ws, chunk, sizeof(chunk), 1000);
        int payload = esp_transport_ws_get_read_payload_len(s_ws);
        if (n < 0 || (n == 0 && (payload != 0 || s_message.in_frame()))) {
            gadget_ws_close(); return -1;
        }
        auto result = s_message.append(esp_transport_ws_get_read_opcode(s_ws),
                                      esp_transport_ws_get_fin_flag(s_ws), payload, chunk, n);
        using Result = GadgetWsMessage::Result;
        if (result == Result::Invalid || result == Result::Closed) {
            gadget_ws_close(); return -1;
        }
        if (result == Result::Ping) {
            const auto pong = static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN);
            int written = esp_transport_ws_send_raw(s_ws, pong, s_message.control,
                                                   s_message.control_size, 1000);
            if (written != static_cast<int>(s_message.control_size)) { gadget_ws_close(); return -1; }
        }
        if (result != Result::Message) return 0;
        s_ready = true;
    }
    size_t count = s_message.size - s_delivered;
    if (count > capacity) count = capacity;
    memcpy(data, s_message.data + s_delivered, count);
    s_delivered += count;
    if (s_delivered == s_message.size) {
        s_message.reset(); s_delivered = 0; s_ready = false;
    }
    return count;
}
