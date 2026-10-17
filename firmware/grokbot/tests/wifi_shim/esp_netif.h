#pragma once
#include "esp_event.h"
struct esp_ip4_addr_t { uint32_t addr; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip, netmask, gw; };
struct ip_event_got_ip_t { esp_netif_ip_info_t ip_info; };
struct esp_netif_t {};
constexpr int ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED = 10;
int esp_netif_init();
esp_netif_t *esp_netif_create_default_wifi_sta();
int esp_netif_dhcpc_stop(esp_netif_t *);
int esp_netif_dhcpc_start(esp_netif_t *);
int esp_netif_set_ip_info(esp_netif_t *, const esp_netif_ip_info_t *);
int esp_netif_set_old_ip_info(esp_netif_t *, const esp_netif_ip_info_t *);
int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);

esp_netif_t *esp_netif_create_default_wifi_ap();
int esp_netif_dhcps_stop(esp_netif_t *);
int esp_netif_dhcps_start(esp_netif_t *);
int esp_netif_str_to_ip4(const char *, esp_ip4_addr_t *);

enum esp_netif_dhcp_status_t { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED };
bool esp_netif_is_netif_up(esp_netif_t *);
int esp_netif_dhcps_get_status(esp_netif_t *, esp_netif_dhcp_status_t *);
