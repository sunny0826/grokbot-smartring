#pragma once
#include "wifi_setup.h"
struct WifiSetupNetwork { char ssid[33] = {}; char password[65] = {}; };
enum class WifiDriverResult { Pending, ScanReady, Connected, Authentication, NotFound, Error };
// Each begin stops STA, waits for STOP and a default event-loop fence, clears
// DHCP state, then starts the requested operation. All calls are owner-only.
bool wifi_driver_scan();
bool wifi_driver_join(const WifiSetupNetwork &network);
WifiDriverResult wifi_driver_poll();
uint8_t wifi_driver_results(WifiSetupAp *destination, uint8_t capacity);
bool wifi_driver_connected(const WifiSetupNetwork &network);
void wifi_driver_clear_scan();
int64_t wifi_setup_now_ms();

// Generate while radio is running, then keep this profile across each STOP/fence.
bool wifi_driver_phone_create(WifiPhoneDisplay &display);
void wifi_driver_phone_disable();
bool wifi_driver_phone_ready();
bool wifi_driver_phone_stopped();
bool wifi_driver_phone_overlap();
