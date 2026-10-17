#include "wifi_setup_http.h"
#include "wifi_setup_http_protocol.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include <cstring>
#include <cstdio>
#include <new>
#include <cerrno>

extern const char page_start[] asm("_binary_wifi_setup_page_html_start");
extern const char page_end[] asm("_binary_wifi_setup_page_html_end");
extern const char script_start[] asm("_binary_wifi_setup_page_js_start");
extern const char script_end[] asm("_binary_wifi_setup_page_js_end");
extern const char style_start[] asm("_binary_wifi_setup_page_css_start");
extern const char style_end[] asm("_binary_wifi_setup_page_css_end");

namespace {
struct Context {
    uint32_t operation;
    char token[33] = {};
    char response[WIFI_HTTP_STATUS_LIMIT] = {};
};
httpd_handle_t server = nullptr;
Context *context = nullptr;
constexpr int64_t request_timeout_us = 3000000;
struct Connection { int64_t deadline; };
void free_connection(void *pointer) { delete static_cast<Connection *>(pointer); }
Connection *io_window(httpd_handle_t handle, int socket, int option) {
    auto *connection = static_cast<Connection *>(httpd_sess_get_transport_ctx(handle, socket));
    if (!connection) return nullptr;
    const int64_t remaining = connection->deadline - esp_timer_get_time();
    // lwIP truncates timeval microseconds to milliseconds; zero would mean no timeout.
    if (remaining < 1000) return nullptr;
    const int64_t wait = remaining < 1000000 ? remaining : 1000000;
    timeval timeout{};
    timeout.tv_sec = wait / 1000000;
    timeout.tv_usec = wait % 1000000;
    if (setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout))) return nullptr;
    return connection;
}
int io_result(const Connection &connection, int count) {
    if (esp_timer_get_time() >= connection.deadline) return HTTPD_SOCK_ERR_FAIL;
    if (count < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
                          HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
    return count;
}
int receive(httpd_handle_t handle, int socket, char *buffer, size_t length, int flags) {
    const auto *connection = io_window(handle, socket, SO_RCVTIMEO);
    if (!connection || !buffer) return HTTPD_SOCK_ERR_FAIL;
    // Raw socket recv: httpd_socket_recv would recursively invoke this override.
    return io_result(*connection, static_cast<int>(recv(socket, buffer, length, flags)));
}
int transmit(httpd_handle_t handle, int socket, const char *buffer, size_t length, int flags) {
    const auto *connection = io_window(handle, socket, SO_SNDTIMEO);
    if (!connection || !buffer) return HTTPD_SOCK_ERR_FAIL;
    return io_result(*connection, static_cast<int>(send(socket, buffer, length, flags)));
}

bool socket_allowed(int socket) {
    auto *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK ||
        !wifi_http_sta_separate(ntohl(info.ip.addr), ntohl(info.netmask.addr))) return false;
    sockaddr_in local{}, peer{};
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
    return getsockname(socket, reinterpret_cast<sockaddr *>(&local), &local_size) == 0 &&
           getpeername(socket, reinterpret_cast<sockaddr *>(&peer), &peer_size) == 0 &&
           local.sin_family == AF_INET && peer.sin_family == AF_INET &&
           wifi_http_address_allowed(ntohl(local.sin_addr.s_addr), ntohl(peer.sin_addr.s_addr));
}
esp_err_t open_socket(httpd_handle_t handle, int socket) {
    if (!socket_allowed(socket)) return ESP_FAIL;
    auto *connection = new (std::nothrow) Connection{esp_timer_get_time() + request_timeout_us};
    if (!connection) return ESP_FAIL;
    httpd_sess_set_transport_ctx(handle, socket, connection, free_connection);
    // IDF deletes the session (and its transport context) if this registration fails.
    if (httpd_sess_set_recv_override(handle, socket, receive) != ESP_OK) return ESP_FAIL;
    return httpd_sess_set_send_override(handle, socket, transmit);
}
void preserve_context(void *) {}
void headers(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "img-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    httpd_resp_set_hdr(request, "Connection", "close");
}
esp_err_t reply(httpd_req_t *request, const char *status, const char *body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    const auto sent = httpd_resp_send(request, body, std::strlen(body));
    // Close every socket, including unread rejected bodies, to prevent request desynchronization.
    return sent == ESP_OK ? ESP_FAIL : sent;
}
bool header(httpd_req_t *request, const char *name, char *value, size_t capacity, bool required) {
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (!length) { value[0] = '\0'; return !required; }
    return length < capacity && httpd_req_get_hdr_value_str(request, name, value, capacity) == ESP_OK;
}
bool active(const Context &ctx, const WifiSetupSnapshot &snapshot) {
    return snapshot.operation == ctx.operation && snapshot.method == WifiSetupMethod::Phone &&
           snapshot.phone_phase == WifiPhonePhase::Serving && snapshot.phase != WifiSetupPhase::Closed &&
           snapshot.remaining_seconds > 0;
}
bool authorize(httpd_req_t *request, bool token) {
    char host[32], origin[48], site[24], supplied[40];
    if (!socket_allowed(httpd_req_to_sockfd(request)) ||
        !header(request, "Host", host, sizeof(host), true) ||
        !header(request, "Origin", origin, sizeof(origin), false) ||
        !header(request, "Sec-Fetch-Site", site, sizeof(site), false) ||
        !wifi_http_origin_allowed(host, origin[0] ? origin : nullptr, site[0] ? site : nullptr)) return false;
    if (!token) return true;
    if (!header(request, "X-Setup-Token", supplied, sizeof(supplied), true)) return false;
    const bool matches = wifi_http_token_matches(static_cast<Context *>(request->user_ctx)->token, supplied);
    wifi_http_wipe(supplied, sizeof(supplied));
    return matches;
}
esp_err_t handle(httpd_req_t *request) {
    headers(request);
    const bool command_route = std::strcmp(request->uri, "/api/command") == 0;
    const bool status_route = std::strcmp(request->uri, "/api/status") == 0;
    if (!authorize(request, command_route || status_route))
        return reply(request, "403 Forbidden", "{\"error\":\"forbidden\"}");
    const auto *connection = static_cast<Connection *>(httpd_sess_get_transport_ctx(request->handle, httpd_req_to_sockfd(request)));
    if (!connection || esp_timer_get_time() >= connection->deadline)
        return reply(request, "408 Request Timeout", "{\"error\":\"request_timeout\"}");
    auto &ctx = *static_cast<Context *>(request->user_ctx);
    const auto snapshot = wifi_setup_snapshot();
    if (!active(ctx, snapshot)) return reply(request, "410 Gone", "{\"error\":\"closed\"}");
    if (command_route) {
        char type[48];
        if (!header(request, "Content-Type", type, sizeof(type), true) ||
            (std::strcmp(type, "application/json") && std::strcmp(type, "application/json; charset=utf-8")))
            return reply(request, "415 Unsupported Media Type", "{\"error\":\"content_type\"}");
        if (!request->content_len || request->content_len > WIFI_HTTP_BODY_LIMIT)
            return reply(request, "413 Payload Too Large", "{\"error\":\"body_size\"}");
        char body[WIFI_HTTP_BODY_LIMIT + 1] = {};
        size_t received = 0;
        const auto deadline = connection->deadline;
        bool complete = true;
        while (received < request->content_len) {
            if (esp_timer_get_time() >= deadline) { complete = false; break; }
            const int count = httpd_req_recv(request, body + received, request->content_len - received);
            if (count <= 0 || esp_timer_get_time() >= deadline) { complete = false; break; }
            received += count;
        }
        WifiPhoneCommand command{};
        const bool valid = complete && wifi_http_parse_command(body, received, command);
        wifi_http_wipe(body, sizeof(body));
        if (!valid) {
            wifi_http_wipe(&command, sizeof(command));
            return reply(request, complete ? "400 Bad Request" : "408 Request Timeout", "{\"error\":\"invalid_request\"}");
        }
        if (esp_timer_get_time() >= deadline) {
            wifi_http_wipe(&command, sizeof(command));
            return reply(request, "408 Request Timeout", "{\"error\":\"request_timeout\"}");
        }
        if (!socket_allowed(httpd_req_to_sockfd(request))) {
            wifi_http_wipe(&command, sizeof(command));
            return reply(request, "403 Forbidden", "{\"error\":\"forbidden\"}");
        }
        // The owner performs all revision/phase/list checks atomically. This snapshot is only a service gate.
        const auto result = command.operation == ctx.operation ? wifi_setup_phone_submit(command) : WifiPhoneReceipt::Closed;
        wifi_http_wipe(&command, sizeof(command));
        switch (result) {
            case WifiPhoneReceipt::Accepted: return reply(request, "202 Accepted", "{\"result\":\"accepted\"}");
            case WifiPhoneReceipt::Stale: return reply(request, "409 Conflict", "{\"error\":\"stale\"}");
            case WifiPhoneReceipt::Busy: return reply(request, "409 Conflict", "{\"error\":\"busy\"}");
            case WifiPhoneReceipt::Invalid: return reply(request, "400 Bad Request", "{\"error\":\"invalid\"}");
            case WifiPhoneReceipt::Closed: return reply(request, "410 Gone", "{\"error\":\"closed\"}");
        }
    }
    if (!std::strcmp(request->uri, "/api/session")) {
        std::snprintf(ctx.response, sizeof(ctx.response), "{\"operation\":%lu,\"token\":\"%s\"}",
                      static_cast<unsigned long>(ctx.operation), ctx.token);
        return reply(request, "200 OK", ctx.response);
    }
    if (status_route) {
        if (!wifi_http_serialize(snapshot, ctx.response, sizeof(ctx.response)))
            return reply(request, "500 Internal Server Error", "{\"error\":\"internal\"}");
        return reply(request, "200 OK", ctx.response);
    }
    const char *start = nullptr, *end = nullptr, *type = nullptr;
    if (!std::strcmp(request->uri, "/")) { start = page_start; end = page_end; type = "text/html; charset=utf-8"; }
    if (!std::strcmp(request->uri, "/app.js")) { start = script_start; end = script_end; type = "text/javascript; charset=utf-8"; }
    if (!std::strcmp(request->uri, "/style.css")) { start = style_start; end = style_end; type = "text/css; charset=utf-8"; }
    if (!start) return reply(request, "404 Not Found", "{\"error\":\"not_found\"}");
    httpd_resp_set_type(request, type);
    const auto sent = httpd_resp_send(request, start, end - start - 1);
    return sent == ESP_OK ? ESP_FAIL : sent;
}
}

bool wifi_setup_http_start(uint32_t operation) {
    if (!operation || server || context) return false;
    context = new (std::nothrow) Context{operation};
    if (!context) return false;
    unsigned char random[16];
    esp_fill_random(random, sizeof(random));
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        context->token[2 * i] = hex[random[i] >> 4];
        context->token[2 * i + 1] = hex[random[i] & 15];
    }
    wifi_http_wipe(random, sizeof(random));
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 2;
    config.max_uri_handlers = 6;
    config.max_resp_headers = 6;
    config.stack_size = 6144;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;
    config.open_fn = open_socket;
    config.global_user_ctx = context;
    config.global_user_ctx_free_fn = preserve_context;
    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        wifi_http_wipe(context, sizeof(*context));
        delete context;
        context = nullptr;
        return false;
    }
    const char *paths[] = {"/", "/app.js", "/style.css", "/api/session", "/api/status", "/api/command"};
    for (const char *path : paths) {
        httpd_uri_t route{};
        route.uri = path;
        route.method = !std::strcmp(path, "/api/command") ? HTTP_POST : HTTP_GET;
        route.handler = handle;
        route.user_ctx = context;
        if (httpd_register_uri_handler(server, &route) != ESP_OK) { wifi_setup_http_stop(); return false; }
    }
    return true;
}
bool wifi_setup_http_stop() {
    if (server && httpd_stop(server) != ESP_OK) return false;
    server = nullptr;
    if (context) { wifi_http_wipe(context, sizeof(*context)); delete context; context = nullptr; }
    return true;
}
