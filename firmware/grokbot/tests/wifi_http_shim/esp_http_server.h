#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
using httpd_handle_t = void *;
enum httpd_method_t { HTTP_GET, HTTP_POST };
struct httpd_req_t {
    httpd_handle_t handle;
    httpd_method_t method;
    const char *uri;
    size_t content_len;
    void *user_ctx;
};
struct httpd_uri_t {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
};
struct httpd_config_t {
    uint16_t max_open_sockets = 7, max_uri_handlers = 8, max_resp_headers = 8;
    size_t stack_size = 4096;
    uint16_t recv_wait_timeout = 5, send_wait_timeout = 5;
    bool lru_purge_enable = false;
    esp_err_t (*open_fn)(httpd_handle_t, int) = nullptr;
    void *global_user_ctx = nullptr;
    void (*global_user_ctx_free_fn)(void *) = nullptr;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, int);
int httpd_req_recv(httpd_req_t *, char *, size_t);
int httpd_req_to_sockfd(httpd_req_t *);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);

constexpr int HTTPD_SOCK_ERR_FAIL = -1;
constexpr int HTTPD_SOCK_ERR_INVALID = -2;
constexpr int HTTPD_SOCK_ERR_TIMEOUT = -3;
using httpd_recv_func_t = int (*)(httpd_handle_t, int, char *, size_t, int);
using httpd_free_ctx_fn_t = void (*)(void *);
esp_err_t httpd_sess_set_recv_override(httpd_handle_t, int, httpd_recv_func_t);
void *httpd_sess_get_transport_ctx(httpd_handle_t, int);
void httpd_sess_set_transport_ctx(httpd_handle_t, int, void *, httpd_free_ctx_fn_t);

using httpd_send_func_t = int (*)(httpd_handle_t, int, const char *, size_t, int);
esp_err_t httpd_sess_set_send_override(httpd_handle_t, int, httpd_send_func_t);
