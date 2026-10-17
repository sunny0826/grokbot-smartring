#include "wifi_setup_http.h"
#include "wifi_setup_http_protocol.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "esp_netif.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cerrno>

namespace {
httpd_config_t config;
std::vector<httpd_uri_t> routes;
std::map<std::string, std::string> request_headers, response_headers;
std::string response, status, body;
size_t offset = 0, chunk = 512;
int64_t now = 0, recv_cost = 0;
bool stop_fails = false, start_fails = false, register_fails = false, recv_fails = false;
bool netif_fails = false;
uint32_t sta_ip = 0xc0a80102, sta_mask = 0xffffff00;
uint32_t local = 0xc0a80401, peer = 0xc0a80402;
WifiSetupSnapshot snapshot;
WifiPhoneReceipt receipt = WifiPhoneReceipt::Accepted;
WifiPhoneCommand submitted;
int submits = 0;
std::string token;
void *transport = nullptr;
httpd_free_ctx_fn_t transport_free = nullptr;
httpd_recv_func_t receiver = nullptr;
httpd_send_func_t sender = nullptr;
int64_t raw_delay = 0, socket_timeout = 0;
int raw_reads = 0;
void invoke(const char *path, const std::string &input = "") {
    response.clear(); response_headers.clear(); status = "200 OK"; body = input; offset = 0;
    // Also call the handler for rejected open() cases to prove per-handler revalidation.
    config.open_fn(reinterpret_cast<void *>(1), 7);
    auto item = std::find_if(routes.begin(), routes.end(), [&](const auto &route) { return !std::strcmp(route.uri, path); });
    assert(item != routes.end());
    httpd_req_t request{reinterpret_cast<void *>(1), item->method, path, input.size(), item->user_ctx};
    // Every handled request asks IDF to close its socket after sending the response.
    assert(item->handler(&request) == ESP_FAIL);
    httpd_sess_set_transport_ctx(request.handle, 7, nullptr, nullptr);
    assert(response_headers.at("Cache-Control") == "no-store");
    assert(response_headers.at("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos);
}
bool parse(const std::string &input) {
    WifiPhoneCommand output{};
    const bool valid = wifi_http_parse_command(input.data(), input.size(), output);
    wifi_http_wipe(&output, sizeof(output));
    return valid;
}
}

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *value) {
    config = *value;
    if (start_fails) return ESP_FAIL;
    *server = reinterpret_cast<void *>(1); routes.clear(); return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t) {
    if (stop_fails) return ESP_FAIL;
    httpd_sess_set_transport_ctx(nullptr, 7, nullptr, nullptr);
    config.global_user_ctx_free_fn(config.global_user_ctx); return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *route) {
    if (register_fails) return ESP_FAIL;
    routes.push_back(*route); return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *key, const char *value) { response_headers[key] = value; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *value) { status = value; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *value) { response_headers["Content-Type"] = value; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *, const char *value, int size) { response.assign(value, size); return ESP_OK; }
int httpd_req_recv(httpd_req_t *, char *output, size_t capacity) {
    now += recv_cost;
    if (recv_fails) return -1;
    const size_t count = std::min({capacity, chunk, body.size() - offset});
    std::memcpy(output, body.data() + offset, count); offset += count; return static_cast<int>(count);
}
int httpd_req_to_sockfd(httpd_req_t *) { return 7; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *key) { return request_headers[key].size(); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *key, char *output, size_t capacity) {
    const auto &value = request_headers[key];
    if (value.size() >= capacity) return ESP_FAIL;
    std::memcpy(output, value.c_str(), value.size() + 1); return ESP_OK;
}
esp_err_t httpd_sess_set_recv_override(httpd_handle_t, int, httpd_recv_func_t callback) { receiver = callback; return ESP_OK; }
esp_err_t httpd_sess_set_send_override(httpd_handle_t, int, httpd_send_func_t callback) { sender = callback; return ESP_OK; }
void *httpd_sess_get_transport_ctx(httpd_handle_t, int) { return transport; }
void httpd_sess_set_transport_ctx(httpd_handle_t, int, void *value, httpd_free_ctx_fn_t free_fn) {
    if (transport && transport != value) transport_free(transport);
    transport = value; transport_free = free_fn;
}
ssize_t test_recv(int, void *output, size_t size, int) {
    ++raw_reads;
    if (raw_delay >= socket_timeout) { now += socket_timeout; errno = EAGAIN; return -1; }
    now += raw_delay;
    if (size) static_cast<char *>(output)[0] = 'a';
    return size ? 1 : 0;
}
ssize_t test_send(int socket, const void *, size_t size, int flags) {
    char byte;
    return test_recv(socket, &byte, size ? 1 : 0, flags);
}
int test_setsockopt(int, int level, int option, const void *value, socklen_t length) {
    assert(level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO) && length == sizeof(timeval));
    const auto *timeout = static_cast<const timeval *>(value);
    socket_timeout = timeout->tv_sec * 1000000 + timeout->tv_usec;
    assert(socket_timeout > 0 && socket_timeout <= 1000000);
    return 0;
}
void esp_fill_random(void *output, size_t size) { static uint8_t seed = 0; auto *bytes = static_cast<uint8_t *>(output); while (size--) *bytes++ = ++seed; }
int64_t esp_timer_get_time() { return now; }
int test_getsockname(int, sockaddr *address, socklen_t *) {
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(address); ipv4->sin_family = AF_INET; ipv4->sin_addr.s_addr = htonl(local); return 0;
}
int test_getpeername(int, sockaddr *address, socklen_t *) {
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(address); ipv4->sin_family = AF_INET; ipv4->sin_addr.s_addr = htonl(peer); return 0;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { static esp_netif_t sta; assert(!std::strcmp(key, "WIFI_STA_DEF")); return &sta; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *info) {
    info->ip.addr = htonl(sta_ip); info->netmask.addr = htonl(sta_mask); return netif_fails ? ESP_FAIL : ESP_OK;
}
WifiSetupSnapshot wifi_setup_snapshot() { return snapshot; }
WifiPhoneReceipt wifi_setup_phone_submit(const WifiPhoneCommand &command) { ++submits; submitted = command; return receipt; }

int main() {
    const std::string scan = R"({"operation":7,"revision":2,"action":"scan"})";
    const std::string connect = R"({"operation":7,"revision":2,"action":"connect","scan_revision":1,"index":0,"password":"fake-pass"})";
    assert(!parse(std::string("{\"x\":") + std::string(240, '[') + "0" + std::string(240, ']') + "}"));
    assert(!parse(R"({"operation":7,"revision":01,"action":"scan"})"));
    assert(!parse(R"({"operation":7,"revision":1.,"action":"scan"})"));
    assert(!parse(R"({"operation":7,"revision":1e0,"action":"scan"})"));
    assert(parse(scan)); assert(parse(connect)); assert(parse(scan + " \r\n"));
    for (const auto &invalid : {
        std::string("[]"), scan + "x", scan + scan, std::string("{\"operation\":true,\"revision\":2,\"action\":\"scan\"}"),
        std::string(R"({"operation":7,"revision":-1,"action":"scan"})"),
        std::string(R"({"operation":7,"revision":1.5,"action":"scan"})"),
        std::string(R"({"operation":4294967296,"revision":2,"action":"scan"})"),
        std::string(R"({"operation":7,"revision":1e999,"action":"scan"})"),
        std::string(R"({"operation":7,"revision":2,"action":"scan","action":"close"})"),
        std::string(R"({"operation":7,"revision":2,"action":"scan","other":1})"),
        std::string(R"({"operation":7,"revision":2,"action":"scan","password":"a"})"),
        std::string(R"({"operation":7,"revision":2,"action":"connect","scan_revision":1,"index":20,"password":"fake-pass"})"),
        std::string(R"({"operation":7,"revision":2,"action":"connect","scan_revision":1,"index":0,"password":null})"),
        std::string(R"({"operation":7,"revision":2,"action":"connect","scan_revision":1,"index":0,"password":"fake\u0000pass"})"),
        std::string(R"({"operation":7,"revision":2,"action":"connect","scan_revision":1,"index":0,"password":"fake\npass"})"),
        std::string(513, ' ')}) assert(!parse(invalid));
    assert(parse(R"({"operation":4294967295,"revision":4294967295,"action":"cancel"})"));
    assert(!parse(scan + std::string(1, '\0')));
    assert(wifi_http_origin_allowed("192.168.4.1", "http://192.168.4.1", "same-origin"));
    assert(wifi_http_origin_allowed("192.168.4.1:80", nullptr, "none"));
    for (const char *host : {"evil.test", "192.168.4.1.evil.test", "192.168.4.1:8080", "192.168.4.1@evil.test"})
        assert(!wifi_http_origin_allowed(host, nullptr, nullptr));
    assert(!wifi_http_origin_allowed("192.168.4.1", "null", nullptr));
    assert(!wifi_http_origin_allowed("192.168.4.1", "https://192.168.4.1", nullptr));
    assert(!wifi_http_origin_allowed("192.168.4.1", nullptr, "cross-site"));
    assert(!wifi_http_origin_allowed("192.168.4.1", nullptr, "same-site"));
    for (uint32_t address : {0xc0a80400u, 0xc0a80401u, 0xc0a804ffu, 0xc0a80102u, 0x7f000001u})
        assert(!wifi_http_address_allowed(0xc0a80401, address));
    assert(!wifi_http_address_allowed(0xc0a80101, 0xc0a80402));
    assert(wifi_http_sta_separate(0, 0));
    assert(wifi_http_sta_separate(0xc0a80102, 0xffffff00));
    assert(!wifi_http_sta_separate(0xc0a80402, 0xffffff00));
    assert(!wifi_http_sta_separate(0xc0a80102, 0xffff0000));
    assert(!wifi_http_sta_separate(0xc0a80102, 0));
    assert(!wifi_http_sta_separate(0xc0a80102, 0xff00ff00));

    snapshot.operation = 7; snapshot.revision = 2; snapshot.method = WifiSetupMethod::Phone;
    snapshot.phase = WifiSetupPhase::Choosing; snapshot.phone_phase = WifiPhonePhase::Serving;
    snapshot.remaining_seconds = 299; snapshot.count = WIFI_SETUP_AP_LIMIT;
    for (auto &ap : snapshot.aps) { std::memset(ap.ssid, 'a', 32); ap.security = WifiSetupSecurity::Personal; ap.rssi = -45; }
    std::strcpy(snapshot.aps[0].ssid, "<script>alert(1)</script>");
    char output[WIFI_HTTP_STATUS_LIMIT];
    assert(wifi_http_serialize(snapshot, output, sizeof(output)));
    assert(std::strstr(output, "<script>") == nullptr);
    auto *json = cJSON_Parse(output); assert(json); assert(cJSON_GetArraySize(cJSON_GetObjectItem(json, "networks")) == 20); cJSON_Delete(json);
    assert(!wifi_http_serialize(snapshot, output, 8));
    assert(wifi_setup_http_start(7)); assert(!wifi_setup_http_start(7));
    assert(config.max_open_sockets == 2 && config.recv_wait_timeout == 1 && config.send_wait_timeout == 1);
    assert(config.open_fn(nullptr, 7) == ESP_OK);
    char byte;
    raw_delay = 800000;
    // A request header making continuous progress must not extend its accepted-socket deadline.
    assert(receiver(nullptr, 7, &byte, 1, 0) == 1 && now == 800000);
    assert(receiver(nullptr, 7, &byte, 1, 0) == 1 && now == 1600000);
    assert(receiver(nullptr, 7, &byte, 1, 0) == 1 && now == 2400000);
    assert(receiver(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL && now == 3000000);
    assert(socket_timeout == 600000 && raw_reads == 4);
    assert(receiver(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL && raw_reads == 4);
    httpd_sess_set_transport_ctx(nullptr, 7, nullptr, nullptr);
    assert(receiver(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL);
    now = 0; raw_reads = 0;
    assert(config.open_fn(nullptr, 7) == ESP_OK);
    // Receive and response writes consume one absolute connection budget.
    assert(receiver(nullptr, 7, &byte, 1, 0) == 1 && now == 800000);
    assert(sender(nullptr, 7, &byte, 1, 0) == 1 && now == 1600000);
    assert(sender(nullptr, 7, &byte, 1, 0) == 1 && now == 2400000);
    assert(sender(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL && now == 3000000);
    assert(socket_timeout == 600000 && raw_reads == 4);
    assert(sender(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL && raw_reads == 4);
    httpd_sess_set_transport_ctx(nullptr, 7, nullptr, nullptr);
    now = 0; raw_reads = 0;
    assert(config.open_fn(nullptr, 7) == ESP_OK);
    // A sub-millisecond timeout rounds to zero (infinite) in IDF lwIP: never start I/O.
    for (int64_t remaining : {int64_t{999}, int64_t{1}}) {
        now = 3000000 - remaining;
        assert(receiver(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL);
        assert(sender(nullptr, 7, &byte, 1, 0) == HTTPD_SOCK_ERR_FAIL);
        assert(raw_reads == 0);
    }
    httpd_sess_set_transport_ctx(nullptr, 7, nullptr, nullptr);
    now = 0; raw_delay = 0;
    local = 0xc0a80101; assert(config.open_fn(nullptr, 7) == ESP_FAIL); local = 0xc0a80401;
    request_headers = {{"Host", "192.168.4.1"}};
    invoke("/api/session"); assert(status == "200 OK");
    json = cJSON_Parse(response.c_str()); token = cJSON_GetObjectItem(json, "token")->valuestring; cJSON_Delete(json); assert(token.size() == 32);
    invoke("/api/status"); assert(status == "403 Forbidden");
    request_headers["X-Setup-Token"] = token;
    invoke("/api/status"); assert(status == "200 OK");
    sta_ip = 0xc0a80402; invoke("/api/status"); assert(status == "403 Forbidden");
    assert(config.open_fn(nullptr, 7) == ESP_FAIL); sta_ip = 0xc0a80102;
    netif_fails = true; invoke("/api/session"); assert(status == "403 Forbidden"); netif_fails = false;
    assert(response.find("password") == std::string::npos && response.find(token) == std::string::npos);
    peer = 0xc0a80102; invoke("/api/status"); assert(status == "403 Forbidden"); peer = 0xc0a80402;
    request_headers["Origin"] = "https://evil.test"; invoke("/api/session"); assert(status == "403 Forbidden"); request_headers.erase("Origin");
    request_headers["Host"] = "evil.test"; invoke("/api/session"); assert(status == "403 Forbidden"); request_headers["Host"] = "192.168.4.1";
    request_headers["X-Setup-Token"] = std::string(32, '0'); invoke("/api/status"); assert(status == "403 Forbidden"); request_headers["X-Setup-Token"] = token;
    invoke("/api/command", scan); assert(status == "415 Unsupported Media Type" && submits == 0);
    request_headers["Content-Type"] = "application/json";
    invoke("/api/command", connect); assert(status == "202 Accepted" && submits == 1 && !std::strcmp(submitted.password, "fake-pass"));
    for (const auto &test : std::vector<std::pair<WifiPhoneReceipt, std::string>>{{WifiPhoneReceipt::Stale,"409 Conflict"},{WifiPhoneReceipt::Busy,"409 Conflict"},{WifiPhoneReceipt::Invalid,"400 Bad Request"},{WifiPhoneReceipt::Closed,"410 Gone"}}) {
        receipt = test.first; invoke("/api/command", scan); assert(status == test.second);
    }
    const int before = submits;
    invoke("/api/command", scan + "junk"); assert(status == "400 Bad Request" && submits == before);
    invoke("/api/command", std::string(513, ' ')); assert(status == "413 Payload Too Large" && submits == before);
    chunk = 1; recv_cost = 1000000; invoke("/api/command", connect); assert(status == "408 Request Timeout" && submits == before && now == 3000000);
    chunk = 512; recv_cost = 0; recv_fails = true; invoke("/api/command", connect); assert(status == "408 Request Timeout"); recv_fails = false;
    snapshot.operation = 8; invoke("/api/status"); assert(status == "410 Gone"); snapshot.operation = 7;
    snapshot.phone_phase = WifiPhonePhase::Closing; invoke("/api/session"); assert(status == "410 Gone"); snapshot.phone_phase = WifiPhonePhase::Serving;
    stop_fails = true; assert(!wifi_setup_http_stop()); assert(!wifi_setup_http_start(8));
    invoke("/api/session"); assert(response.find(token) != std::string::npos);
    stop_fails = false; assert(wifi_setup_http_stop()); assert(wifi_setup_http_stop());
    assert(wifi_setup_http_start(8)); snapshot.operation = 8; invoke("/api/session"); assert(response.find(token) == std::string::npos);
    invoke("/api/status"); assert(status == "403 Forbidden"); assert(wifi_setup_http_stop());
    start_fails = true; assert(!wifi_setup_http_start(8)); start_fails = false;
    register_fails = true; stop_fails = true; assert(!wifi_setup_http_start(8)); assert(!wifi_setup_http_stop());
    stop_fails = false; assert(wifi_setup_http_stop()); register_fails = false;
    assert(wifi_setup_http_start(8)); assert(wifi_setup_http_stop());
    std::cout << "wifi HTTP: parser, serializer, origin/token/socket gates, timeout and lifecycle passed\n";
}
