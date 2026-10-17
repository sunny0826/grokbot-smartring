#pragma once
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK = 0;
using esp_transport_handle_t = void *;
enum ws_transport_opcodes_t { WS_TRANSPORT_OPCODES_PONG = 10, WS_TRANSPORT_OPCODES_FIN = 128 };
struct esp_transport_ws_config_t {
    const char *ws_path, *sub_protocol, *user_agent, *headers, *auth;
    bool propagate_control_frames;
};
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t);
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t, const esp_transport_ws_config_t *);
int esp_transport_connect(esp_transport_handle_t, const char *, int, int);
int esp_transport_close(esp_transport_handle_t);
int esp_transport_destroy(esp_transport_handle_t);
int esp_transport_write(esp_transport_handle_t, const char *, int, int);
int esp_transport_poll_read(esp_transport_handle_t, int);
int esp_transport_read(esp_transport_handle_t, char *, int, int);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t);
int esp_transport_ws_send_raw(esp_transport_handle_t, ws_transport_opcodes_t, const char *, int, int);

int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t);
