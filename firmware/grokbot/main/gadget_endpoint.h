#pragma once
#include "gadget_wifi.h"
#include <cstring>

inline bool gadget_header_value_valid(const char *value)
{
    if (!value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 33 || *p > 126) return false;
    return true;
}

inline bool gadget_remote_host_valid(const char *host)
{
    if (!host || strlen(host) > 127) return false;
    if (!host[0]) return true;
    size_t label = 0;
    bool dot = false, letter = false;
    char previous = 0;
    for (const char *p = host; *p; ++p) {
        bool alpha = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        if (*p == '.') {
            if (!label || previous == '-') return false;
            dot = true;
            label = 0;
        } else {
            if (!alpha && !(*p >= '0' && *p <= '9') && *p != '-') return false;
            if ((!label && *p == '-') || ++label > 63) return false;
            letter |= alpha;
        }
        previous = *p;
    }
    return dot && letter && label && previous != '-';
}

struct GadgetEndpoint {
    const char *host;
    uint16_t control_port;
    uint16_t voice_port;
    bool remote;
};

inline GadgetEndpoint gadget_endpoint(const GadgetWifiCred &cred)
{
    if (cred.remote_host[0]) return {cred.remote_host, 443, 443, true};
    return {cred.host, cred.port, static_cast<uint16_t>(cred.port + 1), false};
}
