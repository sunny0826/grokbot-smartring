#include "wifi_setup_phone_ui.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

extern "C" { LV_FONT_DECLARE(voice_font_22); }
namespace {
lv_obj_t *qr, *caption, *fallback, *countdown;
uint32_t shown_operation;
bool shown_website, encoded;

void text(lv_obj_t *obj, const char *value, int y, int width)
{
    lv_obj_set_width(obj, width);
    lv_label_set_text(obj, value);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
// Wi-Fi QR fields escape delimiters even though generated credentials are alphanumeric.
void field(char *out, size_t capacity, const char *value)
{
    size_t used = 0;
    for (; *value && used + 2 < capacity; ++value) {
        if (std::strchr("\\;,:\"", *value)) out[used++] = '\\';
        out[used++] = *value;
    }
    out[used] = 0;
}
}

void wifi_setup_phone_ui_create(lv_obj_t *parent)
{
    qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, 184);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_set_pos(qr, 88, 67);
    for (auto **out : {&caption, &fallback, &countdown}) {
        *out = lv_label_create(parent);
        lv_obj_set_style_text_align(*out, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(*out, lv_color_hex(0x282823), 0);
        lv_obj_set_style_text_font(*out, &lv_font_montserrat_14, 0);
        lv_obj_remove_flag(*out, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
            LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    }
    lv_obj_set_style_text_font(caption, &voice_font_22, 0);
    wifi_setup_phone_ui_hide();
}
void wifi_setup_phone_ui_hide()
{
    for (auto *obj : {qr, caption, fallback, countdown}) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void wifi_setup_phone_ui_countdown(uint32_t seconds)
{
    char value[40];
    std::snprintf(value, sizeof(value), "%u:%02u", unsigned(seconds / 60), unsigned(seconds % 60));
    text(countdown, value, 333, 120);
}
void wifi_setup_phone_ui_show(const WifiSetupSnapshot &state, bool website)
{
    const auto display = wifi_setup_phone_display(state.operation);
    if (display.phase != WifiPhonePhase::Serving) return;
    text(caption, website ? "2 打开配网页面" : "1 加入临时 Wi-Fi", 35, 240);
    if (!encoded || shown_operation != state.operation || shown_website != website) {
        char payload[180], ssid[67], password[27];
        field(ssid, sizeof(ssid), display.ssid); field(password, sizeof(password), display.password);
        std::snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);
        const char *data = website ? WIFI_PHONE_URL : payload;
        encoded = lv_qrcode_update(qr, data, std::strlen(data)) == LV_RESULT_OK;
        shown_operation = state.operation; shown_website = website;
        std::memset(payload, 0, sizeof(payload)); std::memset(password, 0, sizeof(password));
    }
    if (encoded) lv_obj_remove_flag(qr, LV_OBJ_FLAG_HIDDEN);
    char value[100];
    if (website) std::snprintf(value, sizeof(value), "%s", WIFI_PHONE_URL);
    else std::snprintf(value, sizeof(value), "Wi-Fi: %s\nPASS: %s", display.ssid, display.password);
    text(fallback, value, website ? 263 : 253, 260);
    wifi_setup_phone_ui_countdown(state.remaining_seconds);
}
