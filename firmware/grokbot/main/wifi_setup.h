#pragma once
#include <cstdint>

constexpr uint8_t WIFI_SETUP_AP_LIMIT = 20;
enum class WifiSetupPhase { Closed, Preparing, Scanning, Choosing, Connecting,
                            Saving, Restoring, Connected, Failed };
enum class WifiSetupError { None, VoiceBusy, ScanFailed, NoNetworks, Unsupported,
                            InvalidPassword, Authentication, NotFound, Timeout,
                            Storage, StorageUncertain, Cancelled, Internal };
enum class WifiSetupSecurity { Open, Personal, Unsupported };
enum class WifiSetupMethod { Screen, Phone };
enum class WifiPhonePhase { Off, Starting, Serving, Closing };

constexpr char WIFI_PHONE_ADDRESS[] = "192.168.4.1";
constexpr char WIFI_PHONE_URL[] = "http://192.168.4.1";

struct WifiPhoneDisplay {
    WifiPhonePhase phase = WifiPhonePhase::Off;
    char ssid[33] = {};
    char password[13] = {};
    uint32_t remaining_seconds = 0;
};

enum class WifiPhoneAction { Scan, Connect, Cancel, Close };
enum class WifiPhoneReceipt { Accepted, Stale, Busy, Invalid, Closed };
struct WifiPhoneCommand {
    uint32_t operation = 0;
    uint32_t revision = 0;
    WifiPhoneAction action = WifiPhoneAction::Scan;
    uint32_t scan_revision = 0;
    uint8_t index = 0;
    char password[65] = {};
};

struct WifiSetupAp {
    char ssid[33] = {};
    int8_t rssi = 0;
    WifiSetupSecurity security = WifiSetupSecurity::Unsupported;
};

struct WifiSetupSnapshot {
    uint32_t operation = 0;
    uint32_t revision = 0;
    uint32_t scan_revision = 0;
    WifiSetupPhase phase = WifiSetupPhase::Closed;
    WifiSetupError error = WifiSetupError::None;
    WifiSetupMethod method = WifiSetupMethod::Screen;
    WifiPhonePhase phone_phase = WifiPhonePhase::Off;
    uint32_t remaining_seconds = 0;
    bool command_pending = false;
    bool original_connected = false;
    char connected_ssid[33] = {};
    uint8_t count = 0;
    WifiSetupAp aps[WIFI_SETUP_AP_LIMIT] = {};
};

uint32_t wifi_setup_open(WifiSetupMethod method = WifiSetupMethod::Screen);
bool wifi_setup_scan(uint32_t operation);
bool wifi_setup_connect(uint32_t operation, uint32_t scan_revision,
                        uint8_t index, const char *password);
void wifi_setup_cancel(uint32_t operation);
void wifi_setup_close(uint32_t operation);
WifiSetupSnapshot wifi_setup_snapshot();
WifiPhoneDisplay wifi_setup_phone_display(uint32_t operation);
WifiPhoneReceipt wifi_setup_phone_submit(const WifiPhoneCommand &command);
