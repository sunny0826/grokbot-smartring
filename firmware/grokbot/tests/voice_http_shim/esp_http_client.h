#pragma once
#include <cstdint>
#include "esp_transport_ws.h"
constexpr int ESP_ERR_NOT_FOUND = 2;
struct HttpClient;
using esp_http_client_handle_t = HttpClient *;
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_PUT, HTTP_METHOD_POST };
enum { HTTP_EVENT_ON_HEADER = 1, HTTP_EVENT_DISCONNECTED };
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
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read_response(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
