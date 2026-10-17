#pragma once
#include "esp_transport_ws.h"
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) {host}
#define pdMS_TO_TICKS(ms) (ms)
struct esp_sntp_config_t { const char *server; };
int esp_netif_sntp_init(const esp_sntp_config_t *);
int esp_netif_sntp_sync_wait(int);
