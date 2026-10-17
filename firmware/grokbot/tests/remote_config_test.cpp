#include "gadget_endpoint.h"
#include "nvs.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
static std::map<std::string,std::string> values;
static uint16_t port;
static int fail_set, fail_commit, fail_read, writes, erases;
static bool init_failure=true;
int nvs_flash_init() { return init_failure ? 2 : 0; }
int nvs_flash_erase() { ++erases; values.clear(); return 0; }
int nvs_open(const char *, int, nvs_handle_t *h) { *h = 1; return 0; }
void nvs_close(nvs_handle_t) {}
int nvs_get_blob(nvs_handle_t, const char *key, void *data, size_t *size)
{
    if (fail_read && --fail_read == 0) return 8;
    auto it = values.find(key); if (it == values.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < it->second.size()) return 4;
    memcpy(data, it->second.data(), it->second.size()); *size = it->second.size(); return 0;
}
int nvs_set_blob(nvs_handle_t, const char *key, const void *data, size_t size)
{
    ++writes;
    if (fail_set && --fail_set == 0) return 8;
    values[key] = std::string(static_cast<const char *>(data), size); return 0;
}
int nvs_erase_key(nvs_handle_t, const char *key) { values.erase(key); return 0; }
int nvs_get_str(nvs_handle_t, const char *key, char *data, size_t *size)
{
    auto it = values.find(key); if (it == values.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size <= it->second.size()) return 4;
    memcpy(data, it->second.c_str(), it->second.size()+1); *size = it->second.size()+1; return 0;
}
int nvs_get_u16(nvs_handle_t, const char *, uint16_t *out) { *out = port; return 0; }
int nvs_set_str(nvs_handle_t, const char *key, const char *data) { values[key] = data; return 0; }
int nvs_set_u16(nvs_handle_t, const char *, uint16_t value) { port = value; return 0; }
int nvs_commit(nvs_handle_t) { if (fail_commit && --fail_commit == 0) return 8; return 0; }
int nvs_erase_all(nvs_handle_t) { values.clear(); return 0; }
int main()
{
    GadgetWifiCred cred = {};
    strcpy(cred.ssid, "home"); strcpy(cred.pass, "oldpassword");
    strcpy(cred.token, "0123456789abcdef"); strcpy(cred.host, "192.168.1.2");
    strcpy(cred.ca, "OLD PRIVATE CA"); cred.port = 8767;
    // Legacy reads are passive. A corrupt new record never falls back to old keys.
    values = {{"ssid",cred.ssid},{"pass",cred.pass},{"token",cred.token},{"host",cred.host},{"ca",cred.ca}};
    port = cred.port;
    GadgetWifiCred out;
    assert(!gadget_wifi_load(out) && erases==0);
    init_failure=false;
    assert(gadget_wifi_load(out)); assert(writes == 0);
    values["network"] = "corrupt"; assert(!gadget_wifi_load(out)); values.erase("network");
    fail_commit = 1;
    assert(gadget_wifi_commit_network("trial", "password") == GadgetNetworkSave::RolledBack);
    assert(!values.count("network")); assert(gadget_wifi_load(out)); assert(!strcmp(out.ssid,"home"));
    assert(gadget_wifi_save(cred)); assert(!values.count("remote_host"));
    assert(values["network"].size() == 100);
    values["network"][0]=2; assert(!gadget_wifi_load(out)); values["network"][0]=1;
    assert(gadget_wifi_load(out)); assert(!out.remote_host[0]);
    assert(gadget_wifi_set_remote("devices.example.com"));
    assert(gadget_wifi_set_network("hotspot", "newpassword"));
    assert(gadget_wifi_load(out)); assert(!strcmp(out.remote_host, "devices.example.com"));
    assert(!strcmp(out.host,cred.host) && out.port == 8767 && !strcmp(out.ca,cred.ca));
    assert(!strcmp(out.token,cred.token) && !strcmp(out.ssid,"hotspot") && !strcmp(out.pass,"newpassword"));
    assert(gadget_wifi_set_ssid("renamed")); assert(gadget_wifi_load(out));
    assert(!strcmp(out.ssid,"renamed") && !strcmp(out.pass,"newpassword"));
    auto before = values;
    fail_set = 1; assert(gadget_wifi_commit_network("trial", "password") == GadgetNetworkSave::RolledBack); assert(values == before);
    fail_commit = 1; assert(gadget_wifi_commit_network("trial", "password") == GadgetNetworkSave::RolledBack); assert(values == before);
    fail_read = 2; assert(gadget_wifi_commit_network("trial", "password") == GadgetNetworkSave::RolledBack); assert(values == before);
    fail_commit = 1; fail_set = 2;
    assert(gadget_wifi_commit_network("trial", "password") == GadgetNetworkSave::Uncertain);
    assert(gadget_wifi_load(out)); assert(!strcmp(out.ssid, "trial") && !strcmp(out.pass, "password"));
    assert(gadget_wifi_save(cred)); assert(gadget_wifi_load(out)); assert(out.remote_host[0]);
    assert(!gadget_wifi_set_remote("bad.example\r\nX:hello"));
    assert(!gadget_wifi_set_network("", "newpassword"));
    assert(!gadget_wifi_set_network("home", "short"));
    std::string ssid(32, 'x'), psk(64, 'a');
    assert(gadget_wifi_set_network(ssid.c_str(), psk.c_str())); assert(gadget_wifi_load(out));
    assert(strlen(out.ssid) == 32 && strlen(out.pass) == 64);
    psk[0] = 'z'; assert(!gadget_wifi_set_network(ssid.c_str(), psk.c_str()));
    assert(gadget_wifi_set_remote("")); assert(gadget_wifi_load(out)); assert(!out.remote_host[0]);
    assert(gadget_endpoint(out).voice_port == 8768);
    assert(gadget_wifi_set_network("open-network", "")); assert(gadget_wifi_load(out)); assert(!out.pass[0]);
    values["token"] = "0123456789abcdef\r\nInjection: yes"; assert(!gadget_wifi_load(out));
    assert(erases == 0);
    puts("Network blob migration, rollback fault injection, boundary lengths and endpoint preservation PASS");
}
