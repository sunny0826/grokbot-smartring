#pragma once
#include "esp_transport_ws.h"
using esp_http_client_handle_t = void *;
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_PUT };
constexpr int HTTP_EVENT_ON_HEADER = 1;
struct esp_http_client_event_t { int event_id; char *header_key; char *header_value; void *user_data; };
struct esp_http_client_config_t {
    const char *url = nullptr, *cert_pem = nullptr;
    esp_err_t (*crt_bundle_attach)(void *) = nullptr;
    esp_err_t (*event_handler)(esp_http_client_event_t *) = nullptr;
    void *user_data = nullptr;
    bool skip_cert_common_name_check = false, disable_auto_redirect = false;
    int timeout_ms = 0, max_authorization_retries = 0, buffer_size = 0, buffer_size_tx = 0;
    esp_http_client_method_t method = HTTP_METHOD_GET;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_cleanup(esp_http_client_handle_t);
