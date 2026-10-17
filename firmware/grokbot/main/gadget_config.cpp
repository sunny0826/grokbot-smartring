#include "gadget_endpoint.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <array>
#include <mutex>

static constexpr char NVS_NS[] = "grok";
static std::mutex s_config_mutex;
using NetworkBlob = std::array<uint8_t, 100>;

static bool ensure_nvs()
{
    static bool done;
    if (!done) done = nvs_flash_init() == ESP_OK;
    return done; // Never erase identities or the voice journal to recover init.
}

static bool valid_network(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t n = strnlen(ssid, 33), p = strnlen(password, 65);
    if (!n || n > 32 || p > 64 || (p && p < 8)) return false;
    for (size_t i = 0; i < p; ++i) {
        unsigned char c = password[i];
        if (c < 32 || c > 126) return false;
        if (p == 64 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static NetworkBlob encode(const char *ssid, const char *password)
{
    NetworkBlob blob{};
    blob[0] = 1; blob[1] = strlen(ssid); blob[2] = strlen(password);
    memcpy(blob.data() + 4, ssid, blob[1]);
    memcpy(blob.data() + 36, password, blob[2]);
    return blob;
}

static bool read_network(nvs_handle_t h, char *ssid, char *password)
{
    NetworkBlob blob{};
    size_t n = blob.size();
    esp_err_t error = nvs_get_blob(h, "network", blob.data(), &n);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        n = 33;
        if (nvs_get_str(h, "ssid", ssid, &n) != ESP_OK) return false;
        n = 65;
        return nvs_get_str(h, "pass", password, &n) == ESP_OK && valid_network(ssid, password);
    }
    if (error != ESP_OK || n != blob.size() || blob[0] != 1 || blob[3] ||
        !blob[1] || blob[1] > 32 || blob[2] > 64) return false;
    memcpy(ssid, blob.data() + 4, blob[1]); ssid[blob[1]] = 0;
    memcpy(password, blob.data() + 36, blob[2]); password[blob[2]] = 0;
    return strlen(ssid) == blob[1] && strlen(password) == blob[2] && valid_network(ssid, password);
}

static bool blob_matches(const NetworkBlob &blob, bool absent)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    NetworkBlob actual{};
    size_t n = actual.size();
    esp_err_t err = nvs_get_blob(h, "network", actual.data(), &n);
    nvs_close(h);
    return absent ? err == ESP_ERR_NVS_NOT_FOUND : err == ESP_OK && n == actual.size() && actual == blob;
}

static GadgetNetworkSave commit_network(const char *ssid, const char *password)
{
    if (!valid_network(ssid, password) || !ensure_nvs()) return GadgetNetworkSave::Uncertain;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return GadgetNetworkSave::Uncertain;
    NetworkBlob old{};
    size_t n = old.size();
    esp_err_t old_error = nvs_get_blob(h, "network", old.data(), &n);
    bool absent = old_error == ESP_ERR_NVS_NOT_FOUND;
    if (!absent && (old_error != ESP_OK || n != old.size())) {
        nvs_close(h); return GadgetNetworkSave::Uncertain;
    }
    NetworkBlob next = encode(ssid, password);
    bool saved = nvs_set_blob(h, "network", next.data(), next.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (saved && blob_matches(next, false)) return GadgetNetworkSave::Saved;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return GadgetNetworkSave::Uncertain;
    esp_err_t rollback = absent ? nvs_erase_key(h, "network") : nvs_set_blob(h, "network", old.data(), old.size());
    bool restored = (rollback == ESP_OK || (absent && rollback == ESP_ERR_NVS_NOT_FOUND)) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return restored && blob_matches(old, absent) ? GadgetNetworkSave::RolledBack : GadgetNetworkSave::Uncertain;
}

bool gadget_wifi_load(GadgetWifiCred &out)
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    out = GadgetWifiCred{};
    auto &c = out;
    bool ok = read_network(h, c.ssid, c.pass);
    auto str = [&](const char *key, char *dst, size_t cap) {
        size_t n = cap;
        if (nvs_get_str(h, key, dst, &n) != ESP_OK || n < 2) ok = false;
    };
    str("token", c.token, sizeof(c.token));
    str("host", c.host, sizeof(c.host));
    str("ca", c.ca, sizeof(c.ca));
    if (nvs_get_u16(h, "port", &c.port) != ESP_OK) ok = false;
    size_t n = sizeof(c.remote_host);
    esp_err_t remote = nvs_get_str(h, "remote_host", c.remote_host, &n);
    if (remote != ESP_OK && remote != ESP_ERR_NVS_NOT_FOUND) ok = false;
    if (!gadget_remote_host_valid(c.remote_host) || !gadget_header_value_valid(c.token)) ok = false;
    nvs_close(h);
    if (!ok) out = GadgetWifiCred{};
    return ok;
}

GadgetNetworkSave gadget_wifi_commit_network(const char *ssid, const char *password)
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    return commit_network(ssid, password);
}

bool gadget_wifi_save(const GadgetWifiCred &cred)
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    if (!valid_network(cred.ssid, cred.pass) || !ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "token", cred.token) == ESP_OK &&
              nvs_set_str(h, "host", cred.host) == ESP_OK &&
              nvs_set_u16(h, "port", cred.port) == ESP_OK &&
              nvs_set_str(h, "ca", cred.ca) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok && commit_network(cred.ssid, cred.pass) == GadgetNetworkSave::Saved;
}

bool gadget_wifi_set_ssid(const char *ssid)
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char old_ssid[33]{}, password[65]{};
    bool ok = read_network(h, old_ssid, password);
    nvs_close(h);
    return ok && commit_network(ssid, password) == GadgetNetworkSave::Saved;
}

void gadget_wifi_erase()
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
}

bool gadget_wifi_set_remote(const char *host)
{
    std::lock_guard<std::mutex> lock(s_config_mutex);
    if (!gadget_remote_host_valid(host) || !ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "remote_host", host) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool gadget_wifi_set_network(const char *ssid, const char *password)
{
    return gadget_wifi_commit_network(ssid, password) == GadgetNetworkSave::Saved;
}
