// Wi-Fi (LAN TLS) transport for the gadget protocol — credentials in NVS,
// Wi-Fi STA join, one pinned-CA TLS socket to the gateway's LAN listener.
//
// Provisioning arrives over the USB console (see gadget.cpp): the gateway's
// `wifi-provision` writes ssid/password/token/host/port/ca in one JSON line.
// The console never prints stored secrets; this header is the only place the
// credential struct crosses a boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct GadgetWifiCred {
    char ssid[33];      // protocol: 1-32 bytes
    char pass[65];      // 8-64 bytes, or empty for an open network
    char token[257];    // enroll token, 16-256 chars; sent in the LAN hello
    char host[128];     // gateway LAN address or NAME.local
    uint16_t port;      // LAN listener port (8767 default)
    char ca[3072];      // pinned CA, PEM, NUL-terminated
};

/* ---- NVS (namespace "grok") ---- */

bool gadget_wifi_load(GadgetWifiCred &out);   // true only when every field is present
bool gadget_wifi_save(const GadgetWifiCred &cred);
// Correct the network name without reading or replacing the saved credentials.
bool gadget_wifi_set_ssid(const char *ssid);
void gadget_wifi_erase();

/* ---- transport lifecycle (gadget task only) ----
 * open() blocks through Wi-Fi join and the TLS handshake; on success the
 * socket carries the same LF-delimited JSON session as USB. read() returns
 * byte count, 0 on timeout, <0 when the link is dead and must be reopened. */

bool gadget_wifi_transport_open(const GadgetWifiCred &cred);
void gadget_wifi_transport_close();
bool gadget_wifi_transport_write(const char *buf, size_t len);
int  gadget_wifi_transport_read(char *buf, size_t cap, uint32_t timeout_ms);

// True between a successful open() and the first observed link failure.
bool gadget_wifi_link_up();
