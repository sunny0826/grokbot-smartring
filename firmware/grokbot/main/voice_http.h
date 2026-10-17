#pragma once
#include "esp_http_client.h"
#include "gadget_wifi.h"
#include "voice_receipt.h"
class VoiceHttp {
public:
    VoiceHttp(const char *path, esp_http_client_method_t method, int timeout_ms = 8000,
              int tx_buffer_size = 1024, const VoiceScope *expected_scope = nullptr);
    ~VoiceHttp();
    VoiceHttp(const VoiceHttp &) = delete;
    VoiceHttp &operator=(const VoiceHttp &) = delete;
    esp_http_client_handle_t client = nullptr;
    bool json_response = false;
    bool binary_response = false;
    bool keep_alive = false;
    bool reused_connection = false;
    bool metadata_valid = true;
    unsigned request_id_headers = 0, read_headers = 0;
    char response_id[33] = {};
    bool response_read = false;
    bool legacy_response = false;
    const char *error = "connection_failed";
    bool matches(const VoiceScope *scope) const;
    bool prepare(const char *path);
    void disconnect();
private:
    static esp_err_t on_event(esp_http_client_event_t *event);
    void initialize(const char *path, esp_http_client_method_t method, int timeout_ms, int tx_buffer_size);
    GadgetWifiCred *credentials = nullptr;
    bool configured = false;
    unsigned content_type_headers = 0, connection_headers = 0;
    int64_t created_us = 0, last_us = 0;
    unsigned requests = 0;
};

// A single mailbox worker owns the lease; invalidation never touches a leased handle.
class VoiceHttpSession {
public:
    VoiceHttpSession(const char *path, const VoiceScope *scope);
    ~VoiceHttpSession();
    VoiceHttpSession(const VoiceHttpSession &) = delete;
    VoiceHttpSession &operator=(const VoiceHttpSession &) = delete;
    VoiceHttp *http = nullptr;
    bool accepted = false;
private:
    uint32_t epoch = 0;
    bool acquired = false;
};
void voice_http_invalidate();
