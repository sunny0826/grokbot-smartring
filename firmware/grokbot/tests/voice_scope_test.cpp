#include "voice_scope.h"
#include "gadget_wifi.h"
#include "esp_mac.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static GadgetWifiCred saved;
static int loads;
static bool configured = true, mac_ok = true;
static uint8_t mac_byte = 0xab;
bool gadget_wifi_load(GadgetWifiCred &out) { ++loads; out = saved; return configured; }
esp_err_t esp_read_mac(uint8_t *mac, int)
{ memset(mac, mac_byte, 6); return mac_ok ? ESP_OK : -1; }
static std::string hex(const VoiceScope &scope)
{
    std::string result;
    for (auto byte : scope) { char part[3]; snprintf(part, sizeof(part), "%02x", byte); result += part; }
    return result;
}

int main()
{
    strcpy(saved.token, "enrollment-test-token"); strcpy(saved.ca, "private-ca");
    VoiceScope original{};
    assert(voice_current_scope(original) && loads == 1);
    // Independent Python hashlib vector for the documented v1 encoding.
    assert(hex(original) == "3a61938156f53f99d31d06e871d0e1fd69e48d80156b771b8bace02841abe686");
    assert(voice_scope_matches(original, saved) && loads == 1);
    auto changed = saved;
    strcpy(changed.ssid, "another-network"); strcpy(changed.pass, "other-pass");
    strcpy(changed.host, "192.168.99.2"); changed.port = 1234;
    strcpy(changed.remote_host, "public.example");
    assert(voice_scope_matches(original, changed) && loads == 1);
    changed = saved; strcat(changed.token, "-new"); assert(!voice_scope_matches(original, changed));
    changed = saved; strcat(changed.ca, "-new"); assert(!voice_scope_matches(original, changed));
    mac_byte = 0xac; assert(!voice_scope_matches(original, saved)); mac_byte = 0xab;
    strcpy(saved.token, "abc"); strcpy(saved.ca, "def");
    VoiceScope split{}; assert(voice_current_scope(split));
    strcpy(saved.token, "abcd"); strcpy(saved.ca, "ef");
    assert(!voice_scope_matches(split, saved)); // identical concatenation, different field lengths
    memset(saved.token, 't', sizeof(saved.token) - 1); saved.token[256] = 0;
    memset(saved.ca, 'c', sizeof(saved.ca) - 1); saved.ca[3071] = 0;
    assert(voice_current_scope(split) && voice_scope_matches(split, saved));
    saved.token[256] = 't'; assert(!voice_scope_matches(split, saved)); saved.token[256] = 0;
    saved.ca[3071] = 'c'; assert(!voice_scope_matches(split, saved)); saved.ca[3071] = 0;
    saved.token[0] = 0; assert(!voice_scope_matches(split, saved));
    saved.token[0] = 't'; saved.ca[0] = 0; assert(!voice_scope_matches(split, saved));
    saved.ca[0] = 'c';
    VoiceScope unchanged = original;
    configured = false; assert(!voice_current_scope(unchanged) && unchanged == original);
    configured = true; mac_ok = false;
    assert(!voice_current_scope(unchanged) && unchanged == original);
    puts("Scope v1 known SHA256 vector, snapshot/no reload, identity isolation, route independence and invalid inputs PASS (host crypto adapter)");
}
