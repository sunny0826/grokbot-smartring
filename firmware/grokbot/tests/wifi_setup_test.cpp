#include "wifi_setup.h"
#include "wifi_setup_owner.h"
#include "wifi_setup_driver.h"
#include "wifi_setup_http.h"
#include <arpa/inet.h>
#include "voice.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/queue.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

struct TestQueue { size_t capacity, width; std::deque<std::vector<uint8_t>> items; };
QueueHandle_t xQueueCreate(size_t capacity, size_t width) { return new TestQueue{capacity,width,{}}; }
int xQueueSend(QueueHandle_t q, const void *value, uint32_t)
{
    if (q->items.size() == q->capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(value);
    q->items.emplace_back(bytes, bytes + q->width); return 1;
}
int xQueueReceive(QueueHandle_t q, void *out, uint32_t)
{
    if (q->items.empty()) return 0;
    memcpy(out, q->items.front().data(), q->width); q->items.pop_front(); return 1;
}
struct Handler { esp_event_base_t base; int32_t id; esp_event_handler_t callback; void *arg; };
static std::vector<Handler> handlers;
static void emit(esp_event_base_t base, int32_t id, void *data=nullptr)
{
    for (auto &h : handlers) if (h.base == base && (h.id == id || h.id == ESP_EVENT_ANY_ID)) h.callback(h.arg,base,id,data);
}
int esp_event_loop_create_default() { return 0; }
int esp_event_handler_register(esp_event_base_t base,int32_t id,esp_event_handler_t callback,void *arg)
{ handlers.push_back({base,id,callback,arg}); return 0; }
int esp_event_post(esp_event_base_t base,int32_t id,const void *,size_t,uint32_t) { emit(base,id); return 0; }
static int64_t now;
int64_t esp_timer_get_time() { return now * 1000; }
static esp_netif_t netif, ap_netif;
static esp_netif_ip_info_t ap_ip;
static bool dhcp_ready=true;
bool esp_netif_is_netif_up(esp_netif_t *) { return true; }
int esp_netif_dhcps_get_status(esp_netif_t *, esp_netif_dhcp_status_t *out) { *out=dhcp_ready ? ESP_NETIF_DHCP_STARTED : ESP_NETIF_DHCP_INIT; return 0; }
static esp_netif_ip_info_t ip;
int esp_netif_init() { return 0; }
esp_netif_t *esp_netif_create_default_wifi_sta() { return &netif; }
int esp_netif_dhcpc_stop(esp_netif_t *) { return 0; }
int esp_netif_dhcpc_start(esp_netif_t *) { return 0; }
int esp_netif_set_ip_info(esp_netif_t *iface,const esp_netif_ip_info_t *value) { (iface==&ap_netif ? ap_ip : ip)=*value; return 0; }
int esp_netif_set_old_ip_info(esp_netif_t *,const esp_netif_ip_info_t *) { return 0; }
int esp_netif_get_ip_info(esp_netif_t *iface,esp_netif_ip_info_t *value) { *value=iface==&ap_netif ? ap_ip : ip; return 0; }
esp_netif_t *esp_netif_create_default_wifi_ap() { return &ap_netif; }
int esp_netif_dhcps_stop(esp_netif_t *) { return 0; }
int esp_netif_dhcps_start(esp_netif_t *) { return 0; }
int esp_netif_str_to_ip4(const char *s, esp_ip4_addr_t *out) { return inet_pton(AF_INET,s,&out->addr)==1 ? 0 : 1; }
static wifi_config_t config, ap_config;
static int mode;
static bool ap_first, omit_ap_stop, omit_ap_start;
static bool running, ram, has_ap, stale_on_stop;
static int starts, stops, connects, scans, cleared, writes;
static std::deque<wifi_ap_record_t> aps;
static wifi_ap_record_t current_ap;
int esp_wifi_init(const wifi_init_config_t *) { return 0; }
int esp_wifi_set_storage(int value) { assert(value == WIFI_STORAGE_RAM); ram=true; return 0; }
int esp_wifi_set_mode(int value) { mode=value; return 0; }
int esp_wifi_set_ps(int) { return 0; }
int esp_wifi_stop()
{
    ++stops;
    if (!running) return ESP_ERR_WIFI_NOT_STARTED;
    running=false; has_ap=false;
    ip_event_got_ip_t old{ip};
    if (stale_on_stop) emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&old);
    ip={};
    if (mode == WIFI_MODE_APSTA && ap_first && !omit_ap_stop) emit(WIFI_EVENT,WIFI_EVENT_AP_STOP);
    emit(WIFI_EVENT,WIFI_EVENT_STA_STOP);
    if (mode == WIFI_MODE_APSTA && !ap_first && !omit_ap_stop) emit(WIFI_EVENT,WIFI_EVENT_AP_STOP);
    if (stale_on_stop) {
        emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&old);
        wifi_event_sta_scan_done_t previous{}; emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&previous);
    }
    return 0;
}
int esp_wifi_start() {
    assert(ram); running=true; ++starts;
    if (mode==WIFI_MODE_APSTA && ap_first && !omit_ap_start) emit(WIFI_EVENT,WIFI_EVENT_AP_START);
    emit(WIFI_EVENT,WIFI_EVENT_STA_START);
    if (mode==WIFI_MODE_APSTA && !ap_first && !omit_ap_start) emit(WIFI_EVENT,WIFI_EVENT_AP_START);
    return 0;
}
void esp_fill_random(void *out, size_t size) {
    assert(running); static uint8_t serial=1;
    auto bytes=static_cast<uint8_t *>(out);
    for (size_t i=0;i<size;++i) bytes[i]=serial++;
}
int esp_wifi_set_config(int iface,const wifi_config_t *value) { assert(ram); (iface==WIFI_IF_AP ? ap_config : config)=*value; return 0; }
int esp_wifi_scan_start(const void *,bool block) { assert(!block); ++scans; return 0; }
int esp_wifi_connect() { ++connects; return 0; }
int esp_wifi_scan_stop() { return 0; }
int esp_wifi_clear_ap_list() { ++cleared; aps.clear(); return 0; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) { if (!has_ap) return 1; *out=current_ap; return 0; }
int esp_wifi_scan_get_ap_num(uint16_t *count) { *count=aps.size(); return 0; }
int esp_wifi_scan_get_ap_record(wifi_ap_record_t *out) { if (aps.empty()) return 1; *out=aps.front(); aps.pop_front(); return 0; }
static uint32_t voice_operation;
static VoiceSettingsStatus voice_status = VoiceSettingsStatus::Paused;
static bool voice_busy;
bool voice_settings_pause(uint32_t id) { if (voice_busy) return false; voice_operation=id; return true; }
VoiceSettingsStatus voice_settings_status(uint32_t id) { return id == voice_operation ? voice_status : VoiceSettingsStatus::Inactive; }
void voice_settings_resume(uint32_t id) { if (id == voice_operation) voice_operation=0; }
static GadgetNetworkSave save_result = GadgetNetworkSave::Saved;
static bool cancel_during_save, expire_during_save;
GadgetNetworkSave gadget_wifi_commit_network(const char *ssid,const char *password)
{
    ++writes;
    auto s=wifi_setup_snapshot(); assert(s.phase == WifiSetupPhase::Saving);
    assert(!strcmp(ssid,reinterpret_cast<char *>(config.sta.ssid)) || strlen(ssid)==32);
    assert(strlen(password) <= 64);
    assert(!wifi_setup_usb_acquire());
    if (cancel_during_save) wifi_setup_cancel(s.operation);
    if (expire_during_save) {
        now+=300001;
        WifiPhoneCommand close{}; close.operation=s.operation; close.revision=s.revision; close.action=WifiPhoneAction::Close;
        assert(wifi_setup_phone_submit(close)==WifiPhoneReceipt::Closed);
        assert(wifi_setup_snapshot().phase==WifiSetupPhase::Saving);
    }
    return save_result;
}
static bool http_start_ok=true, http_stop_ok=true, http_live;
static int http_starts, http_stops;
bool wifi_setup_http_start(uint32_t operation) {
    auto s=wifi_setup_snapshot(); assert(s.operation==operation);
    assert(s.phone_phase==WifiPhonePhase::Starting && wifi_driver_phone_ready());
    ++http_starts; http_live=true; return http_start_ok;
}
bool wifi_setup_http_stop() {
    assert(wifi_setup_snapshot().phone_phase==WifiPhonePhase::Closing);
    assert(!wifi_setup_usb_acquire()); ++http_stops;
    if (http_stop_ok) http_live=false;
    return http_stop_ok;
}
static GadgetWifiCred cred;
static void step() { wifi_setup_owner_step(cred); }
static WifiSetupSnapshot snapshot() { return wifi_setup_snapshot(); }
static void add_ap(const char *ssid,int rssi,wifi_auth_mode_t mode=WIFI_AUTH_WPA2_PSK)
{
    wifi_ap_record_t ap{}; memcpy(ap.ssid,ssid,strlen(ssid)); ap.rssi=rssi; ap.authmode=mode; aps.push_back(ap);
}
static void scan_done()
{
    wifi_event_sta_scan_done_t event{}; emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&event); step();
    assert(snapshot().phase == WifiSetupPhase::Choosing);
}
static uint32_t open_scanned(const char *name="new-home")
{
    auto id=wifi_setup_open(); assert(id); step();
    assert(snapshot().phase == WifiSetupPhase::Scanning);
    add_ap(name,-30); scan_done(); return id;
}
static void join(uint32_t id,const char *password="newpassword")
{
    assert(wifi_setup_connect(id,snapshot().scan_revision,0,password)); step();
    assert(snapshot().phase == WifiSetupPhase::Connecting);
}
static void got_ip(bool associate=true,const char *name=nullptr)
{
    current_ap={};
    memcpy(current_ap.ssid,config.sta.ssid,32);
    if (name) { memset(current_ap.ssid,0,33); memcpy(current_ap.ssid,name,strlen(name)); }
    current_ap.authmode=config.sta.threshold.authmode;
    has_ap=true;
    if (associate) emit(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED);
    ip.ip.addr=123; ip.netmask.addr=0xffffff;
    ip_event_got_ip_t acquired{ip}; emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
}
static void restore_success(uint32_t id)
{
    step(); got_ip(); step(); assert(snapshot().phase == WifiSetupPhase::Failed);
    assert(snapshot().original_connected); wifi_setup_close(id); assert(!voice_operation);
}
static uint32_t phone_open()
{
    auto id=wifi_setup_open(WifiSetupMethod::Phone); assert(id); step();
    add_ap("phone-target",-20); scan_done(); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Serving);
    return id;
}
static WifiPhoneCommand command(WifiPhoneAction action)
{
    auto s=snapshot(); WifiPhoneCommand c{};
    c.operation=s.operation; c.revision=s.revision; c.scan_revision=s.scan_revision; c.action=action;
    strcpy(c.password,"phone-password"); return c;
}
static void phone_finish()
{
    for (int i=0;i<4 && wifi_setup_requested();++i) { step(); got_ip(); step(); }
    assert(!wifi_setup_requested() && snapshot().phone_phase==WifiPhonePhase::Off && !http_live && !voice_operation);
    assert(ap_config.ap.password[0]==0);
    assert(mode==WIFI_MODE_STA && wifi_setup_usb_acquire()); wifi_setup_usb_release();
}
static void phone_tests()
{
    for (bool order : {false,true}) {
        ap_first=order;
        auto id=phone_open(); auto profile=ap_config;
        auto display=wifi_setup_phone_display(id);
        assert(strlen(display.password)==12 && strlen(display.ssid)==10 && display.remaining_seconds<=300);
        assert(!wifi_setup_usb_acquire());
        auto connect=command(WifiPhoneAction::Connect);
        assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Accepted);
        assert(snapshot().command_pending);
        assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Stale);
        step();
        assert(!memcmp(&profile.ap,&ap_config.ap,sizeof(profile.ap)));
        got_ip(false); step(); assert(snapshot().phase==WifiSetupPhase::Connecting);
        auto cancel=command(WifiPhoneAction::Cancel);
        assert(wifi_setup_phone_submit(cancel)==WifiPhoneReceipt::Accepted);
        assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Stale);
        step(); step(); got_ip(); step(); assert(snapshot().phase==WifiSetupPhase::Failed);
        assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Stale);
        auto invalidate=command(WifiPhoneAction::Cancel);
        assert(wifi_setup_phone_submit(invalidate)==WifiPhoneReceipt::Accepted); step();
        assert(voice_operation==id && snapshot().phone_phase==WifiPhonePhase::Serving);
        connect=command(WifiPhoneAction::Connect);
        assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Accepted); step(); got_ip(); step();
        assert(snapshot().phase==WifiSetupPhase::Connected && wifi_setup_requested() && voice_operation);
        now+=30001; http_stop_ok=false; step();
        assert(snapshot().phone_phase==WifiPhonePhase::Closing && http_live && voice_operation && !wifi_setup_usb_acquire());
        http_stop_ok=true; phone_finish();
        assert(snapshot().phase==WifiSetupPhase::Connected && !strcmp(cred.ssid,"phone-target"));
        assert(!strcmp(reinterpret_cast<char *>(config.sta.ssid),"phone-target"));
        assert(wifi_setup_usb_acquire());
        wifi_setup_close(id); assert(snapshot().phase==WifiSetupPhase::Closed);
        assert(!wifi_setup_usb_acquire()); wifi_setup_usb_release();
    }
    // Concurrent same-version intent has exactly one winner in the owner mutex.
    auto concurrent_id=phone_open(); auto race=command(WifiPhoneAction::Connect);
    WifiPhoneReceipt first{},second{};
    std::thread a([&] { first=wifi_setup_phone_submit(race); });
    std::thread b([&] { second=wifi_setup_phone_submit(race); });
    a.join(); b.join();
    assert((first==WifiPhoneReceipt::Accepted && second==WifiPhoneReceipt::Stale) ||
           (second==WifiPhoneReceipt::Accepted && first==WifiPhoneReceipt::Stale));
    wifi_setup_close(concurrent_id); phone_finish();
    // Every accepted cancel advances CAS even before owner consumes pending work.
    auto id=phone_open(); auto cancel=command(WifiPhoneAction::Cancel);
    assert(wifi_setup_phone_submit(cancel)==WifiPhoneReceipt::Accepted);
    assert(wifi_setup_phone_submit(cancel)==WifiPhoneReceipt::Stale);
    auto close=command(WifiPhoneAction::Close);
    assert(wifi_setup_phone_submit(close)==WifiPhoneReceipt::Accepted);
    assert(wifi_setup_phone_submit(close)==WifiPhoneReceipt::Closed); phone_finish();
    // Absolute lifetime is not refreshed by reads or commands.
    id=phone_open(); auto stale=command(WifiPhoneAction::Scan); now+=300001;
    assert(wifi_setup_phone_submit(stale)==WifiPhoneReceipt::Closed); phone_finish();
    // AP/STA overlap closes the service but a successfully saved result survives.
    id=phone_open(); auto connect=command(WifiPhoneAction::Connect);
    assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Accepted); step(); got_ip();
    esp_netif_str_to_ip4("192.168.4.20",&ip.ip); esp_netif_str_to_ip4("255.255.255.0",&ip.netmask);
    ip_event_got_ip_t acquired{ip}; emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired); step();
    assert(snapshot().phase==WifiSetupPhase::Connected && snapshot().phone_phase==WifiPhonePhase::Closing);
    phone_finish(); wifi_setup_close(id);
    // HTTP partial startup failure is stopped without holding the owner mutex.
    http_start_ok=false;
    id=wifi_setup_open(WifiSetupMethod::Phone); step(); add_ap("x",-20); scan_done(); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Closing); phone_finish(); http_start_ok=true;
    // No AP_START cannot publish credentials/HTTP; cleanup must still stop APSTA.
    omit_ap_start=true;
    id=wifi_setup_open(WifiSetupMethod::Phone); step(); add_ap("x",-20); scan_done(); step();
    auto before=http_starts; now+=3001; step(); assert(http_starts==before);
    omit_ap_start=false; phone_finish();
    // AP_START alone is insufficient: DHCP must actually be running before HTTP starts.
    dhcp_ready=false; before=http_starts;
    id=wifi_setup_open(WifiSetupMethod::Phone); step(); add_ap("x",-20); scan_done(); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Starting && http_starts==before);
    now+=15001; step(); assert(snapshot().phone_phase==WifiPhonePhase::Closing);
    dhcp_ready=true; phone_finish(); wifi_setup_close(id);
    // Missing AP_STOP prevents fence/restart until both stop events are proven.
    id=phone_open(); auto before_starts=starts;
    omit_ap_stop=true; assert(wifi_setup_phone_submit(command(WifiPhoneAction::Scan))==WifiPhoneReceipt::Accepted);
    step(); assert(starts==before_starts);
    emit(WIFI_EVENT,WIFI_EVENT_AP_STOP); omit_ap_stop=false; step(); assert(starts==before_starts+1);
    add_ap("new",-20); scan_done(); wifi_setup_close(id); phone_finish();
    // Saving is indivisible even when the absolute expiry occurs in the NVS call.
    id=phone_open(); connect=command(WifiPhoneAction::Connect);
    assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Accepted); step(); got_ip();
    expire_during_save=true; step(); expire_during_save=false;
    assert(snapshot().phase==WifiSetupPhase::Connected); phone_finish(); wifi_setup_close(id);
    // Initial and restored network overlaps both close admission.
    esp_netif_str_to_ip4("192.168.4.42",&ip.ip); esp_netif_str_to_ip4("255.255.255.0",&ip.netmask);
    before=http_starts; id=wifi_setup_open(WifiSetupMethod::Phone); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Closing && http_starts==before); phone_finish(); wifi_setup_close(id);
    id=phone_open(); connect=command(WifiPhoneAction::Connect);
    assert(wifi_setup_phone_submit(connect)==WifiPhoneReceipt::Accepted); step();
    wifi_event_sta_disconnected_t auth{WIFI_REASON_AUTH_FAIL}; emit(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&auth); step(); step(); got_ip();
    esp_netif_str_to_ip4("192.168.4.23",&ip.ip); esp_netif_str_to_ip4("255.255.255.0",&ip.netmask);
    acquired.ip_info=ip; emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Closing); phone_finish(); wifi_setup_close(id);
    // Empty original network still serves AP and exits to a stopped STA cleanly.
    memset(cred.ssid,0,sizeof(cred.ssid)); memset(cred.pass,0,sizeof(cred.pass));
    id=phone_open(); wifi_setup_close(id); phone_finish();
    // A missing radio AP_START cannot be mistaken for readiness; an unexpected AP_STOP closes service.
    id=phone_open(); emit(WIFI_EVENT,WIFI_EVENT_AP_STOP); step();
    assert(snapshot().phone_phase==WifiPhonePhase::Closing); phone_finish(); wifi_setup_close(id);
    assert(http_starts>0 && http_stops>0);
}
int main()
{
    strcpy(cred.ssid,"home"); strcpy(cred.pass,"oldpassword");
    strcpy(cred.token,"unchanged-token"); strcpy(cred.remote_host,"devices.example.com");
    // Voice gate ACK is mandatory; cancellation and deadline never touch STA.
    voice_status=VoiceSettingsStatus::Pending;
    auto id=wifi_setup_open(); assert(id); step(); assert(starts==0);
    wifi_setup_close(id); assert(snapshot().phase==WifiSetupPhase::Closed && !voice_operation);
    id=wifi_setup_open(); now+=25001; step(); assert(snapshot().error==WifiSetupError::VoiceBusy && starts==0);
    wifi_setup_close(id); voice_status=VoiceSettingsStatus::Paused;
    voice_busy=true; id=wifi_setup_open(); step(); assert(starts==0); wifi_setup_close(id); voice_busy=false;
    // Refresh immediately rejects old row identity; sorting/dedup and result cleanup.
    id=wifi_setup_open(); step();
    add_ap("same",-80); add_ap("same",-20); add_ap("same",-10,WIFI_AUTH_OPEN);
    add_ap("enterprise",-2,WIFI_AUTH_WPA2_ENTERPRISE); add_ap("wep",-3,WIFI_AUTH_WEP);
    for (int i=0;i<25;++i) add_ap(("ap"+std::to_string(i)).c_str(),-30-i);
    scan_done(); assert(snapshot().count==20 && aps.empty());
    auto revision=snapshot().scan_revision;
    assert(snapshot().aps[0].security==WifiSetupSecurity::Unsupported);
    assert(!wifi_setup_connect(id,revision,0,"password"));
    assert(wifi_setup_scan(id)); assert(!wifi_setup_connect(id,revision,3,"password")); step();
    add_ap("new-home",-30); scan_done();
    join(id); assert(writes==0);
    wifi_event_sta_disconnected_t auth{WIFI_REASON_AUTH_FAIL}; emit(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&auth);
    step(); assert(snapshot().phase==WifiSetupPhase::Restoring && writes==0);
    restore_success(id); assert(snapshot().error==WifiSetupError::None); // Closed has no error.
    // DHCP timeout and unavailable original are bounded, with retry/close available.
    id=open_scanned(); join(id); now+=15001; step(); assert(snapshot().error==WifiSetupError::Timeout);
    step(); now+=15001; step(); assert(snapshot().phase==WifiSetupPhase::Failed && !snapshot().original_connected);
    wifi_setup_close(id); assert(writes==0);
    // Same-SSID trial: old IP before STOP, after STOP, after fence but before association cannot save.
    stale_on_stop=true;
    id=open_scanned("home"); join(id); got_ip(false); step(); assert(writes==0);
    assert(snapshot().phase==WifiSetupPhase::Connecting);
    // A stale IP after a fresh association still needs a matching live netif address.
    ip={}; emit(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED);
    ip_event_got_ip_t obsolete{}; obsolete.ip_info.ip.addr=456;
    emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&obsolete); step(); assert(writes==0);
    // Cancellation wins over successful IP already waiting in the event queue.
    got_ip(); wifi_setup_cancel(id); step(); assert(snapshot().phase==WifiSetupPhase::Restoring && writes==0);
    restore_success(id); stale_on_stop=false;
    // Wrong AP identity with an IP cannot commit.
    id=open_scanned(); join(id); got_ip(true,"wrong-ap"); step(); assert(writes==0);
    wifi_setup_close(id); step(); step(); got_ip(); step(); assert(snapshot().phase==WifiSetupPhase::Closed);
    // Saving is the cancellation linearization point; successful RAM credentials update.
    id=open_scanned(); join(id); got_ip(); cancel_during_save=true; step(); cancel_during_save=false;
    assert(writes==1 && snapshot().phase==WifiSetupPhase::Connected && !wifi_setup_requested());
    assert(!strcmp(cred.ssid,"new-home") && !strcmp(cred.pass,"newpassword"));
    assert(!strcmp(cred.token,"unchanged-token") && !strcmp(cred.remote_host,"devices.example.com"));
    assert(voice_operation==id && !wifi_setup_usb_acquire());
    wifi_setup_close(id); assert(!voice_operation);
    // Both storage error classes restore the previous network and remain distinguishable.
    for (auto result : {GadgetNetworkSave::RolledBack,GadgetNetworkSave::Uncertain}) {
        save_result=result; id=open_scanned("other"); join(id); got_ip(); step();
        assert(snapshot().error==(result==GadgetNetworkSave::RolledBack ? WifiSetupError::Storage : WifiSetupError::StorageUncertain));
        restore_success(id); assert(!strcmp(cred.ssid,"new-home"));
    }
    save_result=GadgetNetworkSave::Saved;
    // Exact wire boundaries, with no strlcpy truncation.
    std::string full_ssid(32,'s'), psk(64,'a');
    id=open_scanned(full_ssid.c_str()); join(id,psk.c_str());
    assert(!memcmp(config.sta.ssid,full_ssid.data(),32) && !memcmp(config.sta.password,psk.data(),64));
    got_ip(); step(); assert(snapshot().phase==WifiSetupPhase::Connected); wifi_setup_close(id);
    // Queue overflow fails closed, clears scan results, restores with a deadline.
    id=open_scanned(); join(id);
    for (int i=0;i<30;++i) emit(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED);
    step(); assert(snapshot().phase==WifiSetupPhase::Restoring); restore_success(id);
    // USB mutation and settings race on one atomic lease.
    for (int i=0;i<100;++i) {
        uint32_t opened=0; bool usb=false;
        std::thread a([&] { opened=wifi_setup_open(); });
        std::thread b([&] { usb=wifi_setup_usb_acquire(); });
        a.join(); b.join(); assert(bool(opened)!=usb);
        if (opened) wifi_setup_close(opened); else wifi_setup_usb_release();
    }
    phone_tests();
    assert(cleared>scans && aps.empty());
    puts("Production Wi-Fi owner + driver: screen + phone AP lifecycle, CAS, TTL, HTTP failures, overlap, stale IP, save and USB lease PASS");
    return 0;
}
