#pragma once
#include "gadget_wifi.h"

// Called only at the gadget task's transport checkpoints.
bool wifi_setup_owner_step(GadgetWifiCred &credentials);
bool wifi_setup_requested();
// The same atomic lease covers the entire USB mutation, including reboot.
bool wifi_setup_usb_acquire();
void wifi_setup_usb_release();
