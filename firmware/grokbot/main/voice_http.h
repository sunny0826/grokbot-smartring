#pragma once
#include "esp_http_client.h"
#include "gadget_wifi.h"
class VoiceHttp {
public:
    VoiceHttp(const char *path, esp_http_client_method_t method, int timeout_ms = 8000);
    ~VoiceHttp();
    esp_http_client_handle_t client = nullptr;
    bool json_response = false;
    const char *error = "connection_failed";
private:
    static esp_err_t on_event(esp_http_client_event_t *event);
    GadgetWifiCred *credentials = nullptr;
};
