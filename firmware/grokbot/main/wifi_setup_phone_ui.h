#pragma once
#include "wifi_setup.h"
#include "lvgl.h"

// Presentation only: credentials are read from the local-display API.
void wifi_setup_phone_ui_create(lv_obj_t *parent);
void wifi_setup_phone_ui_hide();
void wifi_setup_phone_ui_show(const WifiSetupSnapshot &state, bool website);
void wifi_setup_phone_ui_countdown(uint32_t seconds);
