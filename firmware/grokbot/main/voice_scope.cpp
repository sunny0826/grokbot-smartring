#include "voice_scope.h"
#include "gadget_wifi.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"
#include <cstdio>
#include <cstring>

namespace {
bool field(mbedtls_sha256_context &hash, const char *value, size_t capacity)
{
    const size_t size = strnlen(value, capacity);
    if (!size || size == capacity) return false;
    const uint8_t length[] = {uint8_t(size >> 24), uint8_t(size >> 16),
                              uint8_t(size >> 8), uint8_t(size)};
    return mbedtls_sha256_update(&hash, length, sizeof(length)) == 0 &&
        mbedtls_sha256_update(&hash, reinterpret_cast<const uint8_t *>(value), size) == 0;
}

bool compute(const GadgetWifiCred &credentials, VoiceScope &scope)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    char device[24];
    snprintf(device, sizeof(device), "smartring-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // v1: domain + version byte, then BE32 length-prefixed device, token, saved LAN CA.
    static constexpr uint8_t domain[] = {'v','o','i','c','e','-','s','c','o','p','e',1};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    VoiceScope candidate{};
    const bool ok = mbedtls_sha256_starts(&hash, 0) == 0 &&
        mbedtls_sha256_update(&hash, domain, sizeof(domain)) == 0 &&
        field(hash, device, sizeof(device)) &&
        field(hash, credentials.token, sizeof(credentials.token)) &&
        field(hash, credentials.ca, sizeof(credentials.ca)) &&
        mbedtls_sha256_finish(&hash, candidate.data()) == 0;
    mbedtls_sha256_free(&hash);
    if (ok) scope = candidate;
    return ok;
}
}

bool voice_current_scope(VoiceScope &scope)
{
    auto *credentials = static_cast<GadgetWifiCred *>(
        heap_caps_calloc(1, sizeof(GadgetWifiCred), MALLOC_CAP_SPIRAM));
    if (!credentials) return false;
    const bool ok = gadget_wifi_load(*credentials) && compute(*credentials, scope);
    auto *bytes = reinterpret_cast<volatile uint8_t *>(credentials);
    for (size_t i = 0; i < sizeof(*credentials); ++i) bytes[i] = 0;
    free(credentials);
    return ok;
}

bool voice_scope_matches(const VoiceScope &scope, const GadgetWifiCred &credentials)
{
    VoiceScope actual{};
    return compute(credentials, actual) && actual == scope;
}
