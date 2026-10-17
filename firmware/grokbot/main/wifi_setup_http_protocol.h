#pragma once
#include "wifi_setup.h"
#include <cstddef>

constexpr size_t WIFI_HTTP_BODY_LIMIT = 512;
constexpr size_t WIFI_HTTP_STATUS_LIMIT = 4096;
void wifi_http_wipe(void *data, size_t size);
bool wifi_http_parse_command(const char *body, size_t size, WifiPhoneCommand &command);
bool wifi_http_serialize(const WifiSetupSnapshot &snapshot, char *output, size_t capacity);
bool wifi_http_origin_allowed(const char *host, const char *origin, const char *site);
bool wifi_http_token_matches(const char *expected, const char *actual);
bool wifi_http_address_allowed(uint32_t local_host_order, uint32_t peer_host_order);
bool wifi_http_sta_separate(uint32_t ip_host_order, uint32_t mask_host_order);
