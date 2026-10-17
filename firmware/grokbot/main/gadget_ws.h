#pragma once
#include "gadget_wifi.h"
bool gadget_ws_open(const GadgetWifiCred &cred);
void gadget_ws_close();
bool gadget_ws_write(const char *data, size_t size);
int gadget_ws_read(char *data, size_t capacity, uint32_t timeout_ms);
