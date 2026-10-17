#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
using esp_event_base_t = const char *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 7, ESP_EVENT_ANY_ID = -1;
#define ESP_EVENT_DEFINE_BASE(name) const char *name = #name
inline const char *WIFI_EVENT = "wifi";
inline const char *IP_EVENT = "ip";
constexpr int IP_EVENT_STA_GOT_IP = 1;
int esp_event_loop_create_default();
int esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
int esp_event_post(esp_event_base_t, int32_t, const void *, size_t, uint32_t);
