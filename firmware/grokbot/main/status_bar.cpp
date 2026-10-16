#include "status_bar.h"

#include "bsp/smartring_plus.h"
#include "esp_log.h"
#include "esp_wifi.h"

static lv_obj_t *s_wifi;
static lv_obj_t *s_wifi_slash;
static lv_obj_t *s_battery;
static lv_obj_t *s_charge;
static lv_obj_t *s_percent;
static bool s_battery_available;

static constexpr uint32_t INK = 0x45453f;
static constexpr uint32_t MUTED = 0x99958b;
static constexpr uint32_t GREEN = 0x397b58;
static constexpr uint32_t LOW = 0xb54d3d;

static const char *battery_symbol(int percent)
{
    if (percent <= 10) return LV_SYMBOL_BATTERY_EMPTY;
    if (percent <= 25) return LV_SYMBOL_BATTERY_1;
    if (percent <= 50) return LV_SYMBOL_BATTERY_2;
    if (percent <= 80) return LV_SYMBOL_BATTERY_3;
    return LV_SYMBOL_BATTERY_FULL;
}

static void refresh(lv_timer_t *)
{
    wifi_ap_record_t ap = {};
    const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    bsp_battery_data_t battery = {};
    if (s_battery_available) bsp_battery_get_data(&battery);
    const bool valid = s_battery_available && battery.voltage_v > 0.f &&
                       battery.percent >= 0 && battery.percent <= 100;
    const int percent = valid ? battery.percent : -1;
    const bool charging = valid && battery.charging;

    static bool initialized = false;
    static bool previous_wifi = false, previous_charging = false;
    static int previous_percent = -1;
    if (initialized && connected == previous_wifi && percent == previous_percent &&
        charging == previous_charging) return;
    initialized = true;
    previous_wifi = connected;
    previous_percent = percent;
    previous_charging = charging;

    lv_obj_set_style_text_color(s_wifi, lv_color_hex(connected ? GREEN : MUTED), 0);
    lv_obj_set_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN, connected);
    const uint32_t battery_color = !valid ? MUTED : charging ? GREEN : percent <= 20 ? LOW : INK;
    lv_label_set_text(s_battery, battery_symbol(percent));
    lv_obj_set_style_text_color(s_battery, lv_color_hex(battery_color), 0);
    lv_obj_set_flag(s_charge, LV_OBJ_FLAG_HIDDEN, !charging);
    if (valid) lv_label_set_text_fmt(s_percent, "%d%%", percent);
    else lv_label_set_text(s_percent, "--%");
    lv_obj_set_style_text_color(s_percent, lv_color_hex(battery_color), 0);
    ESP_LOGI("status_bar", "wifi=%s battery=%d%% charging=%d", connected ? "up" : "down",
             percent, charging);
}

static lv_obj_t *label(lv_obj_t *parent, int x, const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(INK), 0);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, 6);
    return obj;
}

void status_bar_create(lv_obj_t *screen, bool battery_available)
{
    s_battery_available = battery_available;
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 136, 28);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xece7db), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 14, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi = label(bar, 10, LV_SYMBOL_WIFI);
    static const lv_point_precise_t slash[] = {{10, 21}, {28, 5}};
    s_wifi_slash = lv_line_create(bar);
    lv_line_set_points(s_wifi_slash, slash, 2);
    lv_obj_set_style_line_color(s_wifi_slash, lv_color_hex(MUTED), 0);
    lv_obj_set_style_line_width(s_wifi_slash, 2, 0);
    lv_obj_set_style_line_rounded(s_wifi_slash, true, 0);

    s_battery = label(bar, 48, LV_SYMBOL_BATTERY_EMPTY);
    s_charge = label(bar, 73, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_charge, lv_color_hex(GREEN), 0);
    s_percent = label(bar, 87, "--%");

    refresh(nullptr);
    lv_timer_create(refresh, 1000, nullptr);
}
