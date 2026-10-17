#include "wifi_setup_ui.h"
#include "wifi_setup.h"
#include "wifi_setup_keyboard.h"
#include "wifi_setup_phone_ui.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

extern "C" { LV_FONT_DECLARE(voice_font_22); }

namespace {
constexpr uint32_t PAPER = 0xece7db, INK = 0x282823, MUTED = 0x69665e, GREEN = 0x397b58;
enum class Page { Method, PhoneJoin, PhoneWeb, List, Detail, Password, Failure };
enum class Pending { None, Scan, Connect };
enum class Action { Phone, Screen, PhoneNext, PhonePrevious, PhoneRetry, Close, Scan, Previous, Next, Select, List, Edit, Connect, Cancel, Key, Mode, Erase, KeysNext };
struct Button {
    lv_obj_t *obj, *label, *detail;
    Action action;
    unsigned value, epoch;
    bool pressed;
};
lv_obj_t *root, *title, *body, *hint;
Button buttons[24];
WifiSetupSnapshot drawn;
WifiSetupKeyboard keyboard;
Page page = Page::List;
uint32_t operation, selected_revision, pending_revision;
Pending pending = Pending::None;
unsigned epoch, used, list_page, selected;
bool active, closing, rejected;
const char *local_error;

void isolate(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
}
void draw();
void refresh(lv_timer_t *);

// Unsupported glyphs and malformed UTF-8 use the complete byte representation.
// Raw AP bytes never round-trip through this presentation string.
bool ssid_text(const char *raw, char *out, size_t capacity)
{
    const size_t size = strnlen(raw, 32);
    bool readable = true;
    for (size_t i = 0; i < size && readable;) {
        const unsigned char first = raw[i];
        unsigned n = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                     first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        uint32_t cp = n == 1 ? first : first & ((1u << (7 - n)) - 1);
        if (!n || i + n > size) { readable = false; break; }
        for (unsigned j = 1; j < n; ++j) {
            const unsigned char c = raw[i+j];
            if ((c & 0xc0) != 0x80) readable = false;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (cp < 32 || cp == 127 || (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
            (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) readable = false;
        lv_font_glyph_dsc_t glyph = {};
        if (!lv_font_get_glyph_dsc(&lv_font_source_han_sans_sc_16_cjk, &glyph, cp, 0) || glyph.is_placeholder) readable = false;
        i += n;
    }
    if (readable) { std::snprintf(out, capacity, "%.*s", int(size), raw); return false; }
    size_t offset = 0;
    for (size_t i = 0; i < size && offset + 3 < capacity; ++i)
        offset += std::snprintf(out + offset, capacity - offset, "%02X ", static_cast<unsigned char>(raw[i]));
    if (offset) out[offset-1] = 0;
    return true;
}
const char *error_text(WifiSetupError error)
{
    switch (error) {
    case WifiSetupError::Authentication: return "认证失败，请检查密码";
    case WifiSetupError::NotFound: return "网络不在附近，请重新扫描";
    case WifiSetupError::Timeout: return "连接超时，请重试";
    case WifiSetupError::ScanFailed: return "扫描失败，请刷新";
    case WifiSetupError::NoNetworks: return "未找到网络，请刷新";
    case WifiSetupError::Storage: return "保存失败，原配置已保留";
    case WifiSetupError::StorageUncertain: return "保存异常，请重新设置";
    case WifiSetupError::Cancelled: return "已取消连接";
    case WifiSetupError::InvalidPassword: return "密码长度或格式有误";
    case WifiSetupError::Unsupported: return "不支持此认证方式";
    case WifiSetupError::VoiceBusy: return "语音任务忙，请稍后重试";
    default: return "网络设置失败，请重试";
    }
}
void label(lv_obj_t *obj, int y, int width, const char *text, const lv_font_t *font = &voice_font_22)
{
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_label_set_text(obj, text);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void add(Action action, const char *text, int x, int y, int w = 64, int h = 44, unsigned value = 0,
         const lv_font_t *font = &voice_font_22)
{
    auto &b = buttons[used++];
    b.action = action; b.value = value; b.pressed = false;
    lv_obj_set_pos(b.obj, x, y); lv_obj_set_size(b.obj, w, h);
    lv_obj_set_style_text_font(b.label, font, 0);
    lv_obj_set_style_text_align(b.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(b.label, w - 4);
    lv_obj_set_height(b.label, LV_SIZE_CONTENT);
    lv_label_set_text(b.label, text); lv_obj_center(b.label);
    lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
}
bool selected_valid()
{
    return selected_revision == drawn.scan_revision && selected < drawn.count;
}
void perform(Action action, unsigned value)
{
    local_error = nullptr;
    switch (action) {
    case Action::Phone:
    case Action::Screen:
        operation = wifi_setup_open(action == Action::Phone ? WifiSetupMethod::Phone : WifiSetupMethod::Screen);
        rejected = !operation;
        page = action == Action::Phone ? Page::PhoneJoin : Page::List;
        break;
    case Action::PhoneNext: page = Page::PhoneWeb; break;
    case Action::PhonePrevious:
    case Action::PhoneRetry: page = Page::PhoneJoin; break;
    case Action::Close:
        keyboard.clear();
        if (!operation || (drawn.method == WifiSetupMethod::Phone && drawn.phone_phase == WifiPhonePhase::Off &&
                           drawn.phase == WifiSetupPhase::Connected)) { operation = 0; active = false; lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN); return; }
        closing = true; wifi_setup_close(operation); break;
    case Action::Scan:
        keyboard.clear(); page = Page::List; list_page = 0;
        pending_revision = drawn.revision;
        if (wifi_setup_scan(operation)) pending = Pending::Scan;
        else local_error = "请稍后刷新";
        break;
    case Action::Previous: if (list_page) --list_page; break;
    case Action::Next: if ((list_page + 1) * 3 < drawn.count) ++list_page; break;
    case Action::Select:
        selected = value; selected_revision = drawn.scan_revision;
        keyboard.clear(); page = Page::Detail; break;
    case Action::List: keyboard.clear(); page = Page::List; break;
    case Action::Edit:
        page = selected_valid() && drawn.aps[selected].security == WifiSetupSecurity::Open ? Page::Detail : Page::Password;
        break;
    case Action::Connect:
        if (!selected_valid()) { page = Page::List; keyboard.clear(); break; }
        if (drawn.aps[selected].security == WifiSetupSecurity::Personal && !keyboard.valid()) {
            local_error = "需 8-63 字符 / 64 HEX"; break;
        }
        pending_revision = drawn.revision;
        if (wifi_setup_connect(operation, selected_revision, selected, keyboard.password)) pending = Pending::Connect;
        else local_error = "网络已变化";
        break;
    case Action::Cancel: wifi_setup_cancel(operation); break;
    case Action::Key: if (!keyboard.append(static_cast<char>(value))) local_error = "最多 64 字符"; break;
    case Action::Mode: keyboard.next_mode(); break;
    case Action::Erase: keyboard.erase(); break;
    case Action::KeysNext: keyboard.next_page(); break;
    }
    const auto next = wifi_setup_snapshot();
    if (next.operation == operation) {
        drawn = next;
        if (drawn.phase == WifiSetupPhase::Failed && (action == Action::Phone || action == Action::Screen)) page = Page::Failure;
    }
    if (drawn.revision != pending_revision) pending = Pending::None;
    draw();
}
void event(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    auto &b = *static_cast<Button *>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) { b.epoch = epoch; b.pressed = true; }
    if (code == LV_EVENT_PRESS_LOST) b.pressed = false;
    if (code != LV_EVENT_CLICKED && !(code == LV_EVENT_LONG_PRESSED_REPEAT && b.action == Action::Erase)) return;
    const auto now = wifi_setup_snapshot();
    if (b.pressed && b.epoch == epoch && (!operation ||
        (now.operation == operation && now.revision == drawn.revision && now.scan_revision == drawn.scan_revision && now.phase == drawn.phase))) {
        const auto action = b.action; const auto value = b.value;
        perform(action, value);
        // Long-delete may repeat while the same key stays physically held.
        if (code == LV_EVENT_LONG_PRESSED_REPEAT) { b.epoch = epoch; b.pressed = true; }
    } else refresh(nullptr);
    if (code == LV_EVENT_CLICKED) b.pressed = false;
}
void draw()
{
    ++epoch; used = 0;
    for (auto &b : buttons) { b.pressed = false; lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(b.detail, LV_OBJ_FLAG_HIDDEN); }
    for (auto *obj : {title, body, hint}) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    wifi_setup_phone_ui_hide();
    label(title, 39, 220, "Wi-Fi 设置");
    if (page == Page::Method) {
        label(body, 90, 260, "选择配网方式");
        add(Action::Phone, "手机配网", 80, 137, 200, 52);
        add(Action::Screen, "圆屏输入", 80, 201, 200, 52);
        add(Action::Close, "返回", 132, 274, 96);
        return;
    }
    if (rejected) {
        label(body, 133, 264, "暂时无法设置网络\n请先结束录音或稍后重试");
        add(Action::Close, "返回", 132, 270, 96); return;
    }
    const bool phone = drawn.method == WifiSetupMethod::Phone;
    if (closing || (phone && drawn.phone_phase == WifiPhonePhase::Closing)) {
        label(body, 135, 264, phone ? "正在关闭临时热点" : "正在退出\n等待网络恢复"); return;
    }
    if (pending != Pending::None) {
        label(body, 133, 264, pending == Pending::Scan ? "正在扫描附近网络" : "正在连接 Wi-Fi");
        add(pending == Pending::Scan ? Action::Close : Action::Cancel, "取消", 132, 270, 96);
        return;
    }
    if (phone && drawn.phone_phase == WifiPhonePhase::Serving &&
        (drawn.phase == WifiSetupPhase::Choosing || drawn.phase == WifiSetupPhase::Failed) && page != Page::Failure) {
        lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
        wifi_setup_phone_ui_show(drawn, page == Page::PhoneWeb);
        add(Action::Close, "退出", 80, 286, 80);
        add(page == Page::PhoneWeb ? Action::PhonePrevious : Action::PhoneNext,
            page == Page::PhoneWeb ? "上一步" : "下一步", 168, 286, 112);
        return;
    }
    switch (drawn.phase) {
    case WifiSetupPhase::Closed: return;
    case WifiSetupPhase::Preparing:
    case WifiSetupPhase::Scanning:
    case WifiSetupPhase::Connecting:
    case WifiSetupPhase::Restoring:
    case WifiSetupPhase::Saving:
        label(body, 133, 264, drawn.phase == WifiSetupPhase::Preparing ? (phone ? "准备手机配网\n请稍候" : "准备网络设置\n等待当前任务完成") :
              drawn.phase == WifiSetupPhase::Scanning ? "正在扫描附近网络" :
              drawn.phase == WifiSetupPhase::Connecting ? "正在连接 Wi-Fi" :
              drawn.phase == WifiSetupPhase::Saving ? "正在保存网络" : "正在恢复原网络");
        if (drawn.phase != WifiSetupPhase::Saving && drawn.phase != WifiSetupPhase::Restoring)
            add(drawn.phase == WifiSetupPhase::Connecting ? Action::Cancel : Action::Close, "取消", 132, 270, 96);
        return;
    case WifiSetupPhase::Connected:
        keyboard.clear(); label(body, 135, 264, phone && drawn.phone_phase == WifiPhonePhase::Off && !drawn.original_connected ?
            "配置已保存\n当前未连接" : "已连接 Wi-Fi");
        add(Action::Close, "完成", 132, 270, 96); return;
    default: break;
    }
    if (phone) {
        label(body, 110, 264, error_text(drawn.error));
        if (drawn.phone_phase == WifiPhonePhase::Serving && drawn.error != WifiSetupError::VoiceBusy) {
            label(hint, 177, 264, "可返回二维码\n在手机上重试");
            add(Action::Close, "退出", 80, 270, 96);
            add(Action::PhoneRetry, "二维码", 184, 270, 96);
            wifi_setup_phone_ui_countdown(drawn.remaining_seconds);
        } else add(Action::Close, "返回", 132, 270, 96);
        return;
    }
    if ((page == Page::Detail || page == Page::Password) && !selected_valid()) { keyboard.clear(); page = Page::List; }
    if (page == Page::Password) {
        lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
        add(Action::List, "返回", 74, 44, 52);
        add(Action::Connect, "连接", 234, 44, 52);
        char caption[48]; std::snprintf(caption, sizeof(caption), "%s %u/%u", keyboard.mode_name(), keyboard.page+1, keyboard.pages());
        label(title, 56, 100, caption);
        char mask[80]; std::snprintf(mask, sizeof(mask), "%.*s  %u/64", int(keyboard.length > 12 ? 12 : keyboard.length), "************", keyboard.length);
        label(body, 91, 258, local_error ? local_error : mask);
        for (unsigned i = 0; i < 15; ++i) {
            const char c = keyboard.key(i);
            if (!c) continue;
            const char text[] = {c, 0};
            add(Action::Key, c == ' ' ? "空格" : text, 52 + (i%5)*52, 116 + (i/5)*48, 48, 44, c);
        }
        add(Action::Mode, "模式", 80, 266);
        add(Action::Erase, "删除", 148, 266);
        add(Action::KeysNext, "翻页", 216, 266);
        return;
    }
    if (page == Page::Detail) {
        char text[128]; const bool hex = ssid_text(drawn.aps[selected].ssid, text, sizeof(text));
        label(title, 43, 220, hex ? "网络名称 HEX" : "网络名称");
        label(body, 95, 250, text, &lv_font_source_han_sans_sc_16_cjk);
        const auto security = drawn.aps[selected].security;
        label(hint, 205, 260, local_error ? local_error : security == WifiSetupSecurity::Open ? "开放网络，无需密码" :
            security == WifiSetupSecurity::Personal ? "个人网络，需输入密码" : "不支持此认证方式");
        add(Action::List, "返回", 80, 270, 96);
        if (security != WifiSetupSecurity::Unsupported)
            add(security == WifiSetupSecurity::Open ? Action::Connect : Action::Edit,
                security == WifiSetupSecurity::Open ? "连接" : "密码", 184, 270, 96);
        return;
    }
    if (page == Page::Failure) {
        label(body, 105, 264, error_text(drawn.error));
        if (drawn.error == WifiSetupError::VoiceBusy) {
            add(Action::Close, "返回", 132, 270, 96); return;
        }
        label(hint, 173, 264, drawn.original_connected ? "已恢复原网络" : "原网络暂未连接");
        add(Action::Close, "返回", 80, 270);
        add(Action::Scan, "刷新", 148, 270);
        if (selected_valid()) add(Action::Edit, "重试", 216, 270);
        return;
    }
    if (!drawn.count) label(body, 143, 264, local_error ? local_error : "未找到网络，请刷新");
    else {
        if (list_page * 3 >= drawn.count) list_page = 0;
        char caption[48]; std::snprintf(caption, sizeof(caption), "Wi-Fi  %u/%u", list_page+1, (drawn.count+2)/3);
        label(title, 39, 220, caption);
        for (unsigned row = 0; row < 3 && list_page*3+row < drawn.count; ++row) {
            const unsigned index = list_page*3+row;
            char name[128], text[180]; const bool hex = ssid_text(drawn.aps[index].ssid, name, sizeof(name));
            const auto &ap = drawn.aps[index];
            std::snprintf(text, sizeof(text), "%u. %s%s", index+1, hex ? "HEX " : "", name);
            add(Action::Select, text, 48, 92+58*row, 264, 48, index, &lv_font_source_han_sans_sc_16_cjk);
            auto &button = buttons[used-1];
            lv_obj_set_height(button.label, 18);
            lv_obj_align(button.label, LV_ALIGN_TOP_LEFT, 8, 4);
            lv_obj_set_style_text_align(button.label, LV_TEXT_ALIGN_LEFT, 0);
            std::snprintf(text, sizeof(text), "%d dBm / %s", int(ap.rssi),
                ap.security == WifiSetupSecurity::Open ? "开放" : ap.security == WifiSetupSecurity::Personal ? "加密" : "不支持");
            lv_label_set_text(button.detail, text);
            lv_obj_align(button.detail, LV_ALIGN_TOP_LEFT, 8, 24);
            lv_obj_remove_flag(button.detail, LV_OBJ_FLAG_HIDDEN);
        }
    }
    add(Action::Close, "返回", 60, 270, 56);
    add(Action::Scan, "刷新", 120, 270, 56);
    add(Action::Previous, "上页", 180, 270, 56);
    add(Action::Next, "下页", 240, 270, 56);
}
void refresh(lv_timer_t *)
{
    if (!active || !operation) return;
    const auto next = wifi_setup_snapshot();
    if (next.operation != operation) return;
    if (next.phase == WifiSetupPhase::Closed || (closing && next.method == WifiSetupMethod::Phone &&
        next.phone_phase == WifiPhonePhase::Off && next.phase == WifiSetupPhase::Connected)) {
        keyboard.clear(); active = false; operation = 0;
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN); return;
    }
    if (next.revision == drawn.revision && next.phase == drawn.phase && next.scan_revision == drawn.scan_revision &&
        next.phone_phase == drawn.phone_phase && next.command_pending == drawn.command_pending) {
        if (next.remaining_seconds != drawn.remaining_seconds) {
            drawn.remaining_seconds = next.remaining_seconds;
            if (drawn.method == WifiSetupMethod::Phone && drawn.phone_phase == WifiPhonePhase::Serving &&
                (drawn.phase == WifiSetupPhase::Choosing || drawn.phase == WifiSetupPhase::Failed))
                wifi_setup_phone_ui_countdown(drawn.remaining_seconds);
        }
        return;
    }
    if (next.phase == WifiSetupPhase::Failed && drawn.phase != next.phase) page = Page::Failure;
    drawn = next;
    if (drawn.revision != pending_revision) pending = Pending::None;
    local_error = nullptr; draw();
}
} // namespace

void wifi_setup_ui_create()
{
    if (root) return;
    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root); isolate(root);
    lv_obj_set_size(root, 360, 360);
    lv_obj_set_style_bg_color(root, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(root, [](lv_event_t *e) { lv_event_stop_bubbling(e); }, LV_EVENT_ALL, nullptr);
    for (auto **out : {&title, &body, &hint}) {
        *out = lv_label_create(root); isolate(*out);
        lv_obj_set_style_text_color(*out, lv_color_hex(INK), 0);
        lv_obj_set_style_text_align(*out, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(*out, 5, 0);
    }
    for (auto &b : buttons) {
        b.obj = lv_button_create(root); isolate(b.obj);
        lv_obj_set_style_radius(b.obj, 12, 0);
        lv_obj_set_style_shadow_width(b.obj, 0, 0);
        lv_obj_set_style_border_width(b.obj, 0, 0);
        lv_obj_set_style_pad_all(b.obj, 0, 0);
        lv_obj_set_style_bg_color(b.obj, lv_color_hex(INK), 0);
        lv_obj_set_style_bg_color(b.obj, lv_color_hex(GREEN), LV_STATE_PRESSED);
        lv_obj_add_event_cb(b.obj, event, LV_EVENT_ALL, &b);
        b.label = lv_label_create(b.obj); isolate(b.label);
        lv_obj_set_style_text_color(b.label, lv_color_hex(PAPER), 0);
        lv_obj_set_style_text_align(b.label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(b.label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(b.label, LV_SIZE_CONTENT);
        b.detail = lv_label_create(b.obj); isolate(b.detail);
        lv_obj_set_style_text_font(b.detail, &voice_font_22, 0);
        lv_obj_set_style_text_color(b.detail, lv_color_hex(PAPER), 0);
    }
    wifi_setup_phone_ui_create(root);
    lv_timer_create(refresh, 100, nullptr);
}
void wifi_setup_ui_open()
{
    if (active) return;
    wifi_setup_ui_create(); keyboard.clear(); page = Page::Method; list_page = 0;
    selected_revision = 0; selected = WIFI_SETUP_AP_LIMIT; closing = false; local_error = nullptr; pending = Pending::None;
    operation = 0; rejected = false; active = true;
    drawn = WifiSetupSnapshot{};
    lv_obj_move_foreground(root); lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN); draw();
}
bool wifi_setup_ui_active() { return active; }
