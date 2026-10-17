#pragma once
#include <cstdint>

// Lifecycle is called only by the gadget owner, without the settings mutex.
// Handlers submit commands and read snapshots; they never touch Wi-Fi or NVS.
bool wifi_setup_http_start(uint32_t operation);
bool wifi_setup_http_stop();
