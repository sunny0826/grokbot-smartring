#include "wifi_setup_http_protocol.h"
#include "cJSON.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>

void wifi_http_wipe(void *data, size_t size) {
    auto *bytes = static_cast<volatile unsigned char *>(data);
    while (size--) *bytes++ = 0;
}

namespace {
void erase_json(cJSON *node) {
    for (auto *item = node; item; item = item->next) {
        if (item->valuestring) wifi_http_wipe(item->valuestring, std::strlen(item->valuestring));
        if (item->child) erase_json(item->child);
    }
}
struct Json {
    cJSON *value;
    ~Json() { erase_json(value); cJSON_Delete(value); }
};
bool uint32(const cJSON *value, uint32_t &output) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        std::floor(value->valuedouble) != value->valuedouble) return false;
    output = static_cast<uint32_t>(value->valuedouble);
    return true;
}
bool primitive_object(const char *body, size_t size) {
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(body[i]);
        if (!byte) return false;
        if (quoted) {
            if (byte < 0x20) return false;
            if (body[i] == '"') { quoted = false; continue; }
            if (body[i] != '\\') continue;
            if (++i >= size) return false;
            if (body[i] == 'u' && i + 4 < size && std::memcmp(body + i + 1, "0000", 4) == 0)
                return false;
            continue;
        }
        if (body[i] == '"') { quoted = true; continue; }
        // This flat command has only primitive values. Bound cJSON recursion before allocation.
        if (body[i] == '[' || body[i] == ']') return false;
        if (body[i] == '{' && ++depth > 1) return false;
        if (body[i] == '}' && (!depth || --depth != 0)) return false;
        if (body[i] == '-') return false;
        if (body[i] >= '0' && body[i] <= '9') {
            const size_t start = i;
            while (i + 1 < size && body[i + 1] >= '0' && body[i + 1] <= '9') ++i;
            if (i > start && body[start] == '0') return false;
            if (i + 1 < size && !std::strchr(" ,}\t\r\n", body[i + 1])) return false;
        }
    }
    return !quoted && depth == 0;
}
struct Writer {
    char *out;
    size_t capacity;
    size_t used = 0;
    bool ok = true;
    void add(const char *format, ...) {
        if (!ok) return;
        va_list args;
        va_start(args, format);
        const int count = std::vsnprintf(out + used, capacity - used, format, args);
        va_end(args);
        if (count < 0 || static_cast<size_t>(count) >= capacity - used) { ok = false; return; }
        used += count;
    }
    void hex(const char *input) {
        for (size_t i = 0; i < 32 && input[i]; ++i)
            add("%02x", static_cast<unsigned char>(input[i]));
    }
};
const char *phase(WifiSetupPhase value) {
    switch (value) {
        case WifiSetupPhase::Closed: return "closed";
        case WifiSetupPhase::Preparing: return "preparing";
        case WifiSetupPhase::Scanning: return "scanning";
        case WifiSetupPhase::Choosing: return "choosing";
        case WifiSetupPhase::Connecting: return "connecting";
        case WifiSetupPhase::Saving: return "saving";
        case WifiSetupPhase::Restoring: return "restoring";
        case WifiSetupPhase::Connected: return "connected";
        case WifiSetupPhase::Failed: return "failed";
    }
    return "closed";
}
const char *error(WifiSetupError value) {
    switch (value) {
        case WifiSetupError::None: return "none";
        case WifiSetupError::VoiceBusy: return "voice_busy";
        case WifiSetupError::ScanFailed: return "scan_failed";
        case WifiSetupError::NoNetworks: return "no_networks";
        case WifiSetupError::Unsupported: return "unsupported";
        case WifiSetupError::InvalidPassword: return "invalid_password";
        case WifiSetupError::Authentication: return "authentication";
        case WifiSetupError::NotFound: return "not_found";
        case WifiSetupError::Timeout: return "timeout";
        case WifiSetupError::Storage: return "storage";
        case WifiSetupError::StorageUncertain: return "storage_uncertain";
        case WifiSetupError::Cancelled: return "cancelled";
        case WifiSetupError::Internal: return "internal";
    }
    return "internal";
}
const char *phone(WifiPhonePhase value) {
    switch (value) {
        case WifiPhonePhase::Off: return "off";
        case WifiPhonePhase::Starting: return "starting";
        case WifiPhonePhase::Serving: return "serving";
        case WifiPhonePhase::Closing: return "closing";
    }
    return "off";
}
const char *security(WifiSetupSecurity value) {
    switch (value) {
        case WifiSetupSecurity::Open: return "open";
        case WifiSetupSecurity::Personal: return "personal";
        case WifiSetupSecurity::Unsupported: return "unsupported";
    }
    return "unsupported";
}
}

bool wifi_http_parse_command(const char *body, size_t size, WifiPhoneCommand &command) {
    wifi_http_wipe(&command, sizeof(command));
    if (!body || !size || size > WIFI_HTTP_BODY_LIMIT || !primitive_object(body, size)) return false;
    const char *end = nullptr;
    Json json{cJSON_ParseWithLengthOpts(body, size, &end, false)};
    if (!cJSON_IsObject(json.value) || !end) return false;
    while (end < body + size && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != body + size) return false;
    unsigned fields = 0;
    for (auto *item = json.value->child; item; item = item->next) {
        if (!item->string) return false;
        unsigned bit = 0;
        if (!std::strcmp(item->string, "operation")) bit = 1;
        else if (!std::strcmp(item->string, "revision")) bit = 2;
        else if (!std::strcmp(item->string, "action")) bit = 4;
        else if (!std::strcmp(item->string, "scan_revision")) bit = 8;
        else if (!std::strcmp(item->string, "index")) bit = 16;
        else if (!std::strcmp(item->string, "password")) bit = 32;
        else return false;
        if (fields & bit) return false;
        fields |= bit;
    }
    auto get = [&](const char *key) { return cJSON_GetObjectItemCaseSensitive(json.value, key); };
    WifiPhoneCommand parsed{};
    if (!uint32(get("operation"), parsed.operation) || !parsed.operation ||
        !uint32(get("revision"), parsed.revision)) return false;
    const auto *action = get("action");
    if (!cJSON_IsString(action)) return false;
    if (!std::strcmp(action->valuestring, "connect")) {
        parsed.action = WifiPhoneAction::Connect;
        uint32_t index = 0;
        const auto *password = get("password");
        if (fields != 63 || !uint32(get("scan_revision"), parsed.scan_revision) ||
            !uint32(get("index"), index) || index >= WIFI_SETUP_AP_LIMIT ||
            !cJSON_IsString(password)) return false;
        const size_t length = std::strlen(password->valuestring);
        if (length > 64) return false;
        for (size_t i = 0; i < length; ++i)
            if (static_cast<unsigned char>(password->valuestring[i]) < 0x20 || password->valuestring[i] == 0x7f)
                return false;
        parsed.index = static_cast<uint8_t>(index);
        std::memcpy(parsed.password, password->valuestring, length + 1);
    } else {
        if (fields != 7) return false;
        if (!std::strcmp(action->valuestring, "scan")) parsed.action = WifiPhoneAction::Scan;
        else if (!std::strcmp(action->valuestring, "cancel")) parsed.action = WifiPhoneAction::Cancel;
        else if (!std::strcmp(action->valuestring, "close")) parsed.action = WifiPhoneAction::Close;
        else return false;
    }
    command = parsed;
    wifi_http_wipe(&parsed, sizeof(parsed));
    return true;
}

bool wifi_http_serialize(const WifiSetupSnapshot &s, char *output, size_t capacity) {
    if (!output || !capacity || s.count > WIFI_SETUP_AP_LIMIT) return false;
    Writer w{output, capacity};
    w.add("{\"operation\":%lu,\"revision\":%lu,\"scan_revision\":%lu,\"phase\":\"%s\",\"error\":\"%s\","
          "\"phone_phase\":\"%s\",\"remaining_seconds\":%lu,\"command_pending\":%s,"
          "\"original_connected\":%s,\"connected_ssid_hex\":\"",
          static_cast<unsigned long>(s.operation), static_cast<unsigned long>(s.revision),
          static_cast<unsigned long>(s.scan_revision), phase(s.phase), error(s.error), phone(s.phone_phase),
          static_cast<unsigned long>(s.remaining_seconds), s.command_pending ? "true" : "false",
          s.original_connected ? "true" : "false");
    w.hex(s.connected_ssid);
    w.add("\",\"networks\":[");
    for (uint8_t i = 0; i < s.count; ++i) {
        w.add("%s{\"index\":%u,\"ssid_hex\":\"", i ? "," : "", i);
        w.hex(s.aps[i].ssid);
        w.add("\",\"rssi\":%d,\"security\":\"%s\"}", s.aps[i].rssi, security(s.aps[i].security));
    }
    w.add("]}");
    return w.ok;
}

bool wifi_http_origin_allowed(const char *host, const char *origin, const char *site) {
    if (!host || (std::strcmp(host, WIFI_PHONE_ADDRESS) && std::strcmp(host, "192.168.4.1:80"))) return false;
    if (origin && std::strcmp(origin, WIFI_PHONE_URL) && std::strcmp(origin, "http://192.168.4.1:80")) return false;
    return !site || !std::strcmp(site, "same-origin") || !std::strcmp(site, "none");
}
bool wifi_http_token_matches(const char *expected, const char *actual) {
    if (!expected || !actual || std::strlen(expected) != 32 || std::strlen(actual) != 32) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= static_cast<unsigned char>(expected[i] ^ actual[i]);
    return difference == 0;
}
bool wifi_http_address_allowed(uint32_t local, uint32_t peer) {
    return local == 0xc0a80401 && (peer & 0xffffff00) == 0xc0a80400 &&
           (peer & 0xff) >= 2 && (peer & 0xff) <= 254;
}

bool wifi_http_sta_separate(uint32_t ip, uint32_t mask) {
    if (!ip) return true;
    const uint32_t inverse = ~mask;
    if (!mask || (inverse & (inverse + 1))) return false;
    const uint32_t common = mask & 0xffffff00;
    return (ip & common) != (0xc0a80401 & common);
}
