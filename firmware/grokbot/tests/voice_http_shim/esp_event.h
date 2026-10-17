#pragma once
#include <cstdint>
#include "esp_transport_ws.h"
using esp_event_base_t = const char *;
constexpr esp_event_base_t WIFI_EVENT = "wifi";
constexpr int WIFI_EVENT_STA_DISCONNECTED = 1;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
