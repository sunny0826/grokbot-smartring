#pragma once
#include "esp_event.h"
enum wifi_auth_mode_t { WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK };
enum { WIFI_EVENT_STA_STOP, WIFI_EVENT_STA_START, WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_SCAN_DONE, WIFI_EVENT_AP_STOP, WIFI_EVENT_AP_START };
enum { WIFI_REASON_AUTH_FAIL=100, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, WIFI_REASON_HANDSHAKE_TIMEOUT, WIFI_REASON_802_1X_AUTH_FAILED, WIFI_REASON_NO_AP_FOUND };
constexpr int ESP_ERR_WIFI_NOT_STARTED=11, WIFI_STORAGE_RAM=1, WIFI_MODE_STA=1, WIFI_MODE_APSTA=3, WIFI_IF_STA=1, WIFI_IF_AP=2, WIFI_PS_MIN_MODEM=1;
struct wifi_event_sta_disconnected_t { uint16_t reason; };
struct wifi_event_sta_scan_done_t { uint32_t status; };
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_sta_config_t { uint8_t ssid[32], password[64]; struct { wifi_auth_mode_t authmode; } threshold; };
struct wifi_ap_config_t { uint8_t ssid[32], password[64], ssid_len, channel, max_connection; wifi_auth_mode_t authmode; };
union wifi_config_t { wifi_sta_config_t sta; wifi_ap_config_t ap; };
struct wifi_ap_record_t { uint8_t ssid[33]; int8_t rssi; wifi_auth_mode_t authmode; };
int esp_wifi_init(const wifi_init_config_t *);
int esp_wifi_set_storage(int);
int esp_wifi_set_mode(int);
int esp_wifi_set_ps(int);
int esp_wifi_stop();
int esp_wifi_start();
int esp_wifi_set_config(int, const wifi_config_t *);
int esp_wifi_scan_start(const void *, bool);
int esp_wifi_connect();
int esp_wifi_scan_stop();
int esp_wifi_clear_ap_list();
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
int esp_wifi_scan_get_ap_num(uint16_t *);
int esp_wifi_scan_get_ap_record(wifi_ap_record_t *);
