#pragma once
#include "esp_transport_ws.h"
#include <cstdint>
constexpr int ESP_MAC_WIFI_STA = 0;
esp_err_t esp_read_mac(uint8_t *, int);
