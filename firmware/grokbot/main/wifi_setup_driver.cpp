#include "wifi_setup_driver.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cstdio>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstring>

namespace {
ESP_EVENT_DEFINE_BASE(SETUP_FENCE);
enum class EventKind { Stop, ApStop, Start, ApStart, Associated, Ip, Down, Scan, Fence };
struct Event { EventKind kind; uint32_t value; };
enum class Stage { Idle, Stop, Fence, Start, Scan, Join, Online, Failed };
QueueHandle_t events;
esp_netif_t *netif, *ap_netif;
WifiPhoneDisplay phone;
bool ap_enabled, ap_running, ap_radio, ap_configured, sta_stopped, ap_stopped, sta_started;
std::atomic<bool> overflow{false};
Stage stage = Stage::Idle;
WifiSetupNetwork target;
bool scanning, associated, initialized, started;
uint32_t fence_sequence, expected_fence;
int64_t deadline;
WifiDriverResult failure = WifiDriverResult::Error;

void event(void *, esp_event_base_t base, int32_t id, void *data)
{
    Event e{};
    if (base == SETUP_FENCE) e = {EventKind::Fence, static_cast<uint32_t>(id)};
    else if (base == IP_EVENT) e = {EventKind::Ip, static_cast<ip_event_got_ip_t *>(data)->ip_info.ip.addr};
    else {
        switch (id) {
        case WIFI_EVENT_STA_STOP: e.kind = EventKind::Stop; break;
        case WIFI_EVENT_AP_STOP: e.kind = EventKind::ApStop; break;
        case WIFI_EVENT_AP_START: e.kind = EventKind::ApStart; break;
        case WIFI_EVENT_STA_START: e.kind = EventKind::Start; break;
        case WIFI_EVENT_STA_CONNECTED: e.kind = EventKind::Associated; break;
        case WIFI_EVENT_STA_DISCONNECTED:
            e = {EventKind::Down, static_cast<wifi_event_sta_disconnected_t *>(data)->reason}; break;
        case WIFI_EVENT_SCAN_DONE:
            e = {EventKind::Scan, static_cast<wifi_event_sta_scan_done_t *>(data)->status}; break;
        default: return;
        }
    }
    if (xQueueSend(events, &e, 0) != pdTRUE) overflow.store(true);
}
bool init()
{
    if (initialized) return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) return false;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&config) != ESP_OK) return false;
    events = xQueueCreate(24, sizeof(Event));
    if (!events) return false;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, nullptr) != ESP_OK ||
        esp_event_handler_register(SETUP_FENCE, ESP_EVENT_ANY_ID, event, nullptr) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    initialized = true;
    return true;
}
bool fence()
{
    expected_fence = ++fence_sequence;
    stage = Stage::Fence;
    return esp_event_post(SETUP_FENCE, static_cast<int32_t>(expected_fence), nullptr, 0, 0) == ESP_OK;
}
bool begin(bool scan, const WifiSetupNetwork &network)
{
    if (!init()) return false;
    wifi_driver_clear_scan();
    scanning = scan; target = network; associated = false; failure = WifiDriverResult::Error;
    overflow.store(false);
    // Do not clear this queue after stop: STOP may already have been delivered.
    Event discard;
    while (xQueueReceive(events, &discard, 0) == pdTRUE) {}
    stage = Stage::Stop; sta_stopped = !started; ap_stopped = !ap_radio;
    deadline = wifi_setup_now_ms() + 3000;
    if (!started && !ap_radio) return fence();
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) { started = ap_running = ap_radio = false; return fence(); }
    if (err != ESP_OK) { stage = Stage::Failed; return false; }
    return true;
}
bool start_after_fence()
{
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t empty{};
    if (esp_netif_set_ip_info(netif, &empty) != ESP_OK || esp_netif_set_old_ip_info(netif, &empty) != ESP_OK) return false;
    associated = sta_started = false;
    if (!ap_enabled && ap_configured) {
        wifi_config_t cleared{};
        memcpy(cleared.ap.ssid, "disabled", 8); cleared.ap.ssid_len = 8;
        cleared.ap.authmode = WIFI_AUTH_OPEN; cleared.ap.max_connection = 1;
        if (esp_wifi_set_config(WIFI_IF_AP, &cleared) != ESP_OK) return false;
        ap_configured = false;
    }
    if (esp_wifi_set_mode(ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK) return false;
    if (ap_enabled) {
        // Reuse the default AP netif/handlers; only the radio and secret profile are temporary.
        if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
        if (!ap_netif) return false;
        esp_netif_ip_info_t address{};
        if (esp_netif_str_to_ip4(WIFI_PHONE_ADDRESS, &address.ip) != ESP_OK ||
            esp_netif_str_to_ip4("255.255.255.0", &address.netmask) != ESP_OK) return false;
        address.gw = address.ip;
        esp_netif_dhcps_stop(ap_netif);
        if (esp_netif_set_ip_info(ap_netif, &address) != ESP_OK) return false;
        auto dhcp = esp_netif_dhcps_start(ap_netif);
        if (dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return false;
        wifi_config_t ap{};
        memcpy(ap.ap.ssid, phone.ssid, strlen(phone.ssid));
        memcpy(ap.ap.password, phone.password, 12);
        ap.ap.ssid_len = strlen(phone.ssid); ap.ap.channel = 1;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.max_connection = 2;
        auto configured = esp_wifi_set_config(WIFI_IF_AP, &ap);
        volatile unsigned char *secret = ap.ap.password;
        for (size_t i = 0; i < sizeof(ap.ap.password); ++i) secret[i] = 0;
        if (configured != ESP_OK) return false;
        ap_configured = true;
    }
    if (!ap_enabled && !scanning && !target.ssid[0]) { stage = Stage::Idle; return true; }
    wifi_config_t config{};
    memcpy(config.sta.ssid, target.ssid, strnlen(target.ssid, 32));
    memcpy(config.sta.password, target.password, strnlen(target.password, 64));
    config.sta.threshold.authmode = target.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    auto configured = esp_wifi_set_config(WIFI_IF_STA, &config);
    volatile unsigned char *secret = config.sta.password;
    for (size_t i = 0; i < sizeof(config.sta.password); ++i) secret[i] = 0;
    if (configured != ESP_OK) return false;
    esp_err_t dhcp = esp_netif_dhcpc_start(netif);
    if (dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return false;
    stage = Stage::Start;
    started = esp_wifi_start() == ESP_OK;
    ap_radio = started && ap_enabled;
    return started;
}
WifiDriverResult disconnected(uint32_t reason)
{
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_802_1X_AUTH_FAILED)
        return WifiDriverResult::Authentication;
    if (reason == WIFI_REASON_NO_AP_FOUND) return WifiDriverResult::NotFound;
    return WifiDriverResult::Error;
}
WifiSetupSecurity security(wifi_auth_mode_t mode)
{
    if (mode == WIFI_AUTH_OPEN) return WifiSetupSecurity::Open;
    if (mode == WIFI_AUTH_WPA2_PSK || mode == WIFI_AUTH_WPA_WPA2_PSK || mode == WIFI_AUTH_WPA3_PSK ||
        mode == WIFI_AUTH_WPA2_WPA3_PSK) return WifiSetupSecurity::Personal;
    return WifiSetupSecurity::Unsupported;
}
}

int64_t wifi_setup_now_ms() { return esp_timer_get_time() / 1000; }
bool wifi_driver_scan() { return begin(true, {}); }
bool wifi_driver_join(const WifiSetupNetwork &network) { return begin(false, network); }
void wifi_driver_clear_scan()
{
    if (!initialized) return;
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
}
bool wifi_driver_connected(const WifiSetupNetwork &network)
{
    if (!network.ssid[0]) return stage == Stage::Idle;
    if (stage != Stage::Online || !associated) return false;
    wifi_ap_record_t ap{};
    esp_netif_ip_info_t ip{};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
        ip.ip.addr && ip.netmask.addr && strnlen(reinterpret_cast<char *>(ap.ssid), 33) == strlen(network.ssid) &&
        memcmp(ap.ssid, network.ssid, strlen(network.ssid)) == 0 &&
        security(ap.authmode) == (network.password[0] ? WifiSetupSecurity::Personal : WifiSetupSecurity::Open);
}
WifiDriverResult wifi_driver_poll()
{
    if (!initialized) return WifiDriverResult::Error;
    if (overflow.exchange(false)) { stage = Stage::Failed; failure = WifiDriverResult::Error; }
    Event e;
    bool scan_done = false;
    while (xQueueReceive(events, &e, 0) == pdTRUE) {
        if (e.kind == EventKind::ApStop && stage != Stage::Stop) {
            ap_running = ap_radio = false;
            if (ap_enabled) stage = Stage::Failed;
        }
        if (stage == Stage::Stop && (e.kind == EventKind::Stop || e.kind == EventKind::ApStop)) {
            if (e.kind == EventKind::Stop) { sta_stopped = true; started = false; }
            else { ap_stopped = true; ap_running = ap_radio = false; }
            if (sta_stopped && ap_stopped && !fence()) stage = Stage::Failed;
        } else if (stage == Stage::Fence && e.kind == EventKind::Fence && e.value == expected_fence) {
            if (!start_after_fence()) stage = Stage::Failed;
        } else if (stage == Stage::Start && (e.kind == EventKind::Start || e.kind == EventKind::ApStart)) {
            if (e.kind == EventKind::Start) sta_started = true;
            else ap_running = true;
            if (!sta_started || (ap_enabled && !ap_running)) continue;
            stage = scanning ? Stage::Scan : (target.ssid[0] ? Stage::Join : Stage::Idle);
            esp_err_t err = scanning ? esp_wifi_scan_start(nullptr, false) :
                (target.ssid[0] ? esp_wifi_connect() : ESP_OK);
            if (err != ESP_OK) stage = Stage::Failed;
        } else if (stage == Stage::Join && e.kind == EventKind::Associated) associated = true;
        else if (stage == Stage::Join && e.kind == EventKind::Ip && associated) {
            esp_netif_ip_info_t ip{};
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr && ip.ip.addr == e.value) stage = Stage::Online;
        } else if ((stage == Stage::Join || stage == Stage::Online) && e.kind == EventKind::Down) {
            associated = false; stage = Stage::Failed; failure = disconnected(e.value);
        } else if (stage == Stage::Scan && e.kind == EventKind::Scan) {
            if (e.value) stage = Stage::Failed;
            else scan_done = true;
        }
    }
    if ((stage == Stage::Stop || stage == Stage::Fence || stage == Stage::Start) && wifi_setup_now_ms() >= deadline) stage = Stage::Failed;
    if (stage == Stage::Failed) { wifi_driver_clear_scan(); return failure; }
    if (stage == Stage::Online || stage == Stage::Idle) return WifiDriverResult::Connected;
    if (scan_done) return WifiDriverResult::ScanReady;
    return WifiDriverResult::Pending;
}
uint8_t wifi_driver_results(WifiSetupAp *out, uint8_t capacity)
{
    uint8_t used = 0;
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) { wifi_driver_clear_scan(); return 0; }
    for (uint16_t i = 0; i < count; ++i) {
        wifi_ap_record_t record{};
        if (esp_wifi_scan_get_ap_record(&record) != ESP_OK) break;
        size_t length = strnlen(reinterpret_cast<char *>(record.ssid), 33);
        if (!length || length > 32) continue;
        WifiSetupAp ap{};
        memcpy(ap.ssid, record.ssid, length); ap.rssi = record.rssi; ap.security = security(record.authmode);
        uint8_t at = 0;
        while (at < used && (strcmp(out[at].ssid, ap.ssid) || out[at].security != ap.security)) ++at;
        if (at < used) {
            if (out[at].rssi >= ap.rssi) continue;
            for (uint8_t j = at + 1; j < used; ++j) out[j - 1] = out[j];
            --used;
        }
        at = 0;
        while (at < used && out[at].rssi >= ap.rssi) ++at;
        if (at >= capacity) continue;
        if (used < capacity) ++used;
        for (uint8_t j = used - 1; j > at; --j) out[j] = out[j - 1];
        out[at] = ap;
    }
    wifi_driver_clear_scan();
    return used;
}

bool wifi_driver_phone_create(WifiPhoneDisplay &display)
{
    if (!started || ap_enabled) return false;
    uint8_t random[14]; esp_fill_random(random, sizeof(random));
    snprintf(phone.ssid, sizeof(phone.ssid), "MyBot-%02X%02X", random[0], random[1]);
    constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < 12; ++i) {
        uint8_t value = random[i + 2];
        while (value >= 248) esp_fill_random(&value, 1);
        phone.password[i] = alphabet[value % 62];
    }
    volatile uint8_t *secret = random;
    for (size_t i = 0; i < sizeof(random); ++i) secret[i] = 0;
    phone.password[12] = 0; display = phone; ap_enabled = true;
    return true;
}
void wifi_driver_phone_disable()
{
    ap_enabled = false;
    volatile unsigned char *bytes = reinterpret_cast<volatile unsigned char *>(&phone);
    for (size_t i = 0; i < sizeof(phone); ++i) bytes[i] = 0;
}
bool wifi_driver_phone_ready()
{
    if (!ap_enabled || !ap_running || stage == Stage::Start || stage == Stage::Stop || stage == Stage::Fence) return false;
    esp_netif_dhcp_status_t dhcp{};
    esp_netif_ip_info_t ip{}; esp_ip4_addr_t expected{}, mask{};
    return esp_netif_is_netif_up(ap_netif) &&
        esp_netif_dhcps_get_status(ap_netif, &dhcp) == ESP_OK && dhcp == ESP_NETIF_DHCP_STARTED &&
        esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK &&
        esp_netif_str_to_ip4(WIFI_PHONE_ADDRESS, &expected) == ESP_OK &&
        esp_netif_str_to_ip4("255.255.255.0", &mask) == ESP_OK &&
        ip.ip.addr == expected.addr && ip.netmask.addr == mask.addr;
}
bool wifi_driver_phone_stopped() { return !ap_radio && !ap_enabled && !ap_configured && stage != Stage::Stop && stage != Stage::Fence; }
bool wifi_driver_phone_overlap()
{
    if (!netif) return false;
    esp_netif_ip_info_t sta{};
    if (esp_netif_get_ip_info(netif, &sta) != ESP_OK) return true;
    if (!sta.ip.addr) return false;
    if (!sta.netmask.addr) return true;
    esp_ip4_addr_t address{}, mask{};
    if (esp_netif_str_to_ip4(WIFI_PHONE_ADDRESS, &address) != ESP_OK ||
        esp_netif_str_to_ip4("255.255.255.0", &mask) != ESP_OK) return true;
    uint32_t common = sta.netmask.addr & mask.addr;
    return (sta.ip.addr & common) == (address.addr & common);
}
