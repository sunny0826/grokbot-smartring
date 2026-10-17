#pragma once

// Called with the LVGL lock held. The overlay preserves the screen underneath.
void wifi_setup_ui_create();
void wifi_setup_ui_open();
bool wifi_setup_ui_active();
