#include "voice_ui.h"
#include "voice_motion.h"
#include "home_controls.h"

#include "voice.h"
#include "voice_page.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <cstdlib>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include <cstring>

extern "C" {
LV_FONT_DECLARE(voice_font_22);
}

namespace {
constexpr uint32_t PAPER = 0xece7db;
constexpr uint32_t INK = 0x282823;
constexpr uint32_t MUTED = 0x69665e;
constexpr uint32_t RED = 0xb54d3d;
constexpr uint32_t GREEN = 0x397b58;

struct Button {
    lv_obj_t *obj = nullptr;
    lv_obj_t *label = nullptr;
    VoiceAction action = VoiceAction::Close;
    VoiceAction pressed_action = VoiceAction::Close;
    VoicePhase pressed_phase = VoicePhase::Idle;
    uint32_t pressed_generation = 0;
    bool pressed = false;
};

lv_obj_t *root;
lv_obj_t *title;
lv_obj_t *timer_label;
lv_obj_t *hint;
lv_obj_t *dot;
lv_obj_t *level;
VoiceSnapshot drawn;
bool initialized;
bool was_covered;
Button primary;
Button secondary;
Button previous;
Button talk;
uint32_t hold_id, hold_started;
bool holding, hold_cancelled;
lv_obj_t *page_image;
lv_image_dsc_t page_descriptor;
uint8_t *display_page;

void isolate(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
}

void swallow(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

void cancel_hold()
{
    if (holding && !hold_cancelled) voice_hold_end(hold_id, VoiceHoldEnd::Cancel);
    hold_cancelled = true;
    initialized = false;
}

void hold_event(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        if (holding || voice_snapshot().phase != VoicePhase::Idle) break;
        hold_id = voice_hold_begin();
        holding = hold_id != 0;
        hold_started = lv_tick_get();
        hold_cancelled = false;
        initialized = false;
        break;
    case LV_EVENT_PRESSING:
        if (holding && !hold_cancelled && lv_indev_active()) {
            lv_point_t point;
            lv_area_t bounds;
            lv_indev_get_point(lv_indev_active(), &point);
            lv_obj_get_coords(talk.obj, &bounds);
            if (point.x < bounds.x1 || point.x > bounds.x2 || point.y < bounds.y1 || point.y > bounds.y2)
                cancel_hold();
        }
        break;
    case LV_EVENT_RELEASED:
        if (holding && !hold_cancelled)
            voice_hold_end(hold_id, lv_tick_elaps(hold_started) >= 300 ? VoiceHoldEnd::Send : VoiceHoldEnd::Cancel);
        holding = false;
        initialized = false;
        break;
    case LV_EVENT_PRESS_LOST:
        cancel_hold();
        holding = false;
        break;
    case LV_EVENT_DELETE:
        cancel_hold();
        holding = false;
        talk.obj = talk.label = nullptr;
        break;
    default: break;
    }
}

lv_obj_t *make_label(int y, int width, uint32_t color)
{
    lv_obj_t *label = lv_label_create(root);
    isolate(label);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &voice_font_22, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

void button_event(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    auto *button = static_cast<Button *>(lv_event_get_user_data(event));
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        button->pressed_generation = drawn.generation;
        button->pressed_phase = drawn.phase;
        button->pressed_action = button->action;
        button->pressed = true;
        break;
    case LV_EVENT_CLICKED:
        if (button->pressed && button->pressed_generation == drawn.generation &&
            button->pressed_phase == drawn.phase && button->pressed_action == button->action) {
            voice_action(button->pressed_action, button->pressed_generation);
        }
        button->pressed = false;
        break;
    case LV_EVENT_PRESS_LOST:
        button->pressed = false;
        break;
    default:
        break;
    }
}

void make_button(Button &button, lv_obj_t *parent = nullptr)
{
    button.obj = lv_button_create(parent ? parent : root);
    isolate(button.obj);
    lv_obj_set_size(button.obj, 116, 48);
    lv_obj_set_style_radius(button.obj, 24, 0);
    lv_obj_set_style_border_width(button.obj, 0, 0);
    lv_obj_set_style_shadow_width(button.obj, 0, 0);
    lv_obj_set_style_pad_all(button.obj, 0, 0);
    lv_obj_set_style_bg_color(button.obj, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(button.obj, lv_color_hex(MUTED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button.obj, button_event, LV_EVENT_ALL, &button);
    button.label = lv_label_create(button.obj);
    isolate(button.label);
    lv_obj_set_style_text_font(button.label, &voice_font_22, 0);
    lv_obj_set_style_text_color(button.label, lv_color_hex(PAPER), 0);
    lv_obj_center(button.label);
}

void set_button(Button &button, const char *text, VoiceAction action, int x)
{
    lv_obj_set_width(button.obj, 116);
    button.action = action;
    button.pressed = false;
    lv_label_set_text(button.label, text);
    lv_obj_align(button.obj, LV_ALIGN_TOP_MID, x, 276);
    lv_obj_remove_flag(button.obj, LV_OBJ_FLAG_HIDDEN);
}

void show_phase(bool covered)
{
    lv_obj_add_flag(primary.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(secondary.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(previous.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    primary.pressed = secondary.pressed = previous.pressed = false;
    const bool recording = drawn.phase == VoicePhase::Recording;
    const bool audio = recording || drawn.phase == VoicePhase::Ready;
    lv_obj_set_flag(dot, LV_OBJ_FLAG_HIDDEN, !recording);
    lv_obj_set_flag(level, LV_OBJ_FLAG_HIDDEN, !recording);
    lv_obj_set_flag(timer_label, LV_OBJ_FLAG_HIDDEN, !audio);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, audio ? 245 : 218);
    lv_obj_set_style_text_color(title, lv_color_hex(recording ? RED : INK), 0);

    switch (drawn.phase) {
    case VoicePhase::Idle:
        if (holding) {
            lv_label_set_text(title, hold_cancelled ? "已取消" : "准备录音");
            lv_label_set_text(hint, hold_cancelled ? "松开返回" : "看到正在录音后\n再开始说话");
        }
        return;
    case VoicePhase::Preparing:
        lv_label_set_text(title, "准备录音");
        lv_label_set_text(hint, "看到正在录音后\n再开始说话");
        break;
    case VoicePhase::Recording:
        lv_label_set_text(title, "正在录音");
        lv_label_set_text(hint, drawn.hold_id ? "滑出按钮取消" : "再按 BOOT 停止并发送");
        if (!drawn.hold_id) set_button(primary, "取消录音", VoiceAction::Discard, 0);
        break;
    case VoicePhase::Ready:
        lv_label_set_text(title, "录音已就绪");
        lv_label_set_text(hint, drawn.hold_id ? "已录满 30 秒" : "按 BOOT 发送录音");
        if (!drawn.hold_id) set_button(primary, "取消录音", VoiceAction::Discard, 0);
        break;
    case VoicePhase::Uploading:
        lv_label_set_text(title, "正在上传");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        break;
    case VoicePhase::UploadFailed:
        lv_label_set_text(title, "连接状态未确认");
        lv_label_set_text(hint, "点重试继续上传\n返回将清除本机录音");
        set_button(primary, "重试", VoiceAction::Retry, -63);
        set_button(secondary, "返回", VoiceAction::Close, 63);
        break;
    case VoicePhase::Stored:
        lv_label_set_text(title, "等待处理");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    case VoicePhase::Transcribing:
        lv_label_set_text(title, "正在转写");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    case VoicePhase::Waiting:
        lv_label_set_text(title, "等待接单");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    case VoicePhase::Claimed:
        lv_label_set_text(title, "正在处理");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    case VoicePhase::Replied:
        lv_label_set_text(title, "收到回复");
        lv_label_set_text(hint, "查看后可标记已读");
        lv_obj_set_style_text_color(title, lv_color_hex(GREEN), 0);
        set_button(primary, "查看", VoiceAction::Open, -63);
        set_button(secondary, "稍后", VoiceAction::Close, 63);
        break;
    case VoicePhase::NeedsReview:
        lv_label_set_text(title, "任务需要核对");
        lv_label_set_text(hint, "请在 Bot 中查看\n避免重复处理");
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    case VoicePhase::LoadingPage:
        lv_label_set_text(title, "正在读取回复");
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        break;
    case VoicePhase::ReplyFailed:
        lv_label_set_text(title, "回复读取未确认");
        lv_label_set_text(hint, "回复仍保留在 Mac\n可以再次查看");
        set_button(primary, "查看", VoiceAction::Open, -63);
        set_button(secondary, "稍后", VoiceAction::Close, 63);
        break;
    case VoicePhase::ViewingReply:
        if (drawn.error[0]) lv_label_set_text(title, "保存失败");
        else lv_label_set_text_fmt(title, "回复 %u / %u", drawn.page_index + 1, drawn.page_count);
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        if (!display_page) {
#ifdef ESP_PLATFORM
            display_page = static_cast<uint8_t *>(heap_caps_malloc(VOICE_PAGE_BYTES, MALLOC_CAP_SPIRAM));
#else
            display_page = static_cast<uint8_t *>(malloc(VOICE_PAGE_BYTES));
#endif
        }
        if (display_page && voice_copy_page(drawn.generation, display_page, VOICE_PAGE_BYTES)) {
            lv_image_cache_drop(&page_descriptor);
            page_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
            page_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
            page_descriptor.header.w = VOICE_PAGE_WIDTH;
            page_descriptor.header.h = VOICE_PAGE_HEIGHT;
            page_descriptor.header.stride = VOICE_PAGE_WIDTH * 2;
            page_descriptor.data_size = VOICE_PAGE_BYTES;
            page_descriptor.data = display_page;
            lv_image_set_src(page_image, &page_descriptor);
            lv_obj_remove_flag(page_image, LV_OBJ_FLAG_HIDDEN);
            if (!covered) voice_motion_reveal(page_image);
            else voice_motion_finish_reveal(page_image);
            const bool last = drawn.page_index + 1 == drawn.page_count;
            set_button(primary, last ? "已读" : "下页", last ? VoiceAction::MarkRead : VoiceAction::Next, 87);
            lv_obj_set_width(primary.obj, 78);
            if (drawn.page_index) {
                set_button(previous, "上页", VoiceAction::Previous, -87);
                lv_obj_set_width(previous.obj, 78);
            }
        } else {
            lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(hint, "回复暂不可用\n请稍后再试");
        }
        set_button(secondary, "稍后", VoiceAction::Close, 0);
        lv_obj_set_width(secondary.obj, 78);
        break;
    case VoicePhase::Failed:
        lv_label_set_text(title, "语音处理失败");
        lv_label_set_text(hint, "已存至 Mac\n可以重试处理");
        set_button(primary, "重试", VoiceAction::Retry, -63);
        set_button(secondary, "返回", VoiceAction::Close, 63);
        break;
    case VoicePhase::Unavailable:
        if (std::strcmp(drawn.error, "empty_recording") == 0) {
            lv_label_set_text(title, "未录到有效语音");
            lv_label_set_text(hint, "请按返回后重新录音");
        } else {
            lv_label_set_text(title, "暂时无法录音");
            lv_label_set_text(hint, "麦克风暂不可用\n请稍后再试");
        }
        set_button(primary, "返回", VoiceAction::Close, 0);
        break;
    }
}

} // namespace

VoicePresentation voice_ui_update(uint32_t now, bool covered)
{
    if (!root) return {covered ? CharacterMode::Covered : CharacterMode::Home};
    const VoiceSnapshot next = voice_snapshot();
    const bool open_arriving_reply = initialized && !covered &&
        next.phase == VoicePhase::Replied &&
        (drawn.phase == VoicePhase::Uploading || drawn.phase == VoicePhase::Stored ||
         drawn.phase == VoicePhase::Transcribing || drawn.phase == VoicePhase::Waiting ||
         drawn.phase == VoicePhase::Claimed);
    const bool changed = !initialized || drawn.phase != next.phase ||
                         drawn.generation != next.generation ||
                         drawn.hold_id != next.hold_id ||
                         std::strcmp(drawn.error, next.error) != 0;
    const bool time_changed = changed || drawn.duration_ms / 1000 != next.duration_ms / 1000;
    drawn = next;
    initialized = true;
    if (covered && !was_covered) voice_motion_finish_reveal(page_image);
    was_covered = covered;
    lv_obj_set_flag(talk.obj, LV_OBJ_FLAG_HIDDEN,
                    !holding && (drawn.phase != VoicePhase::Idle || !home_controls_visible()));
    const char *talk_text = holding ? (hold_cancelled ? "已取消" : "松开发送") : "按住说话";
    if (std::strcmp(lv_label_get_text(talk.label), talk_text)) lv_label_set_text(talk.label, talk_text);
    lv_obj_set_style_opa(talk.obj, holding ? LV_OPA_COVER : home_controls_opacity(), 0);
    lv_obj_set_flag(root, LV_OBJ_FLAG_HIDDEN, drawn.phase == VoicePhase::Idle && !holding);
    const float amplitude = voice_motion_update(drawn.phase, drawn.peak, now, !covered);
    const bool reading = drawn.phase == VoicePhase::ViewingReply;
    if (changed) lv_obj_set_style_bg_opa(root, reading ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    VoicePresentation view;
    view.phase = drawn.phase;
    view.level = amplitude;
    view.unread = drawn.unread;
    if (holding && drawn.phase == VoicePhase::Idle)
        view.hold = hold_cancelled ? HoldVisual::Cancelled : HoldVisual::Preparing;
    view.mode = covered || reading ? CharacterMode::Covered :
        voice_ui_active() ? CharacterMode::Voice : CharacterMode::Home;
    if (drawn.phase == VoicePhase::Idle && !holding) return view;
    if (changed) show_phase(covered);
    if (open_arriving_reply) voice_action(VoiceAction::Open, drawn.generation);
    if (time_changed) {
        const unsigned seconds = drawn.duration_ms / 1000;
        lv_label_set_text_fmt(timer_label, "%02u:%02u / 00:30", seconds / 60, seconds % 60);
    }
    return view;
}

void voice_ui_create(lv_obj_t *screen)
{
    if (root) return;
    root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    isolate(root);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(root, swallow, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        voice_motion_reset();
        root = nullptr;
        if (talk.obj) lv_obj_delete(talk.obj);
        lv_image_set_src(page_image, nullptr);
        lv_image_cache_drop(&page_descriptor);
        free(display_page);
        display_page = nullptr;
        initialized = false;
        was_covered = false;
    }, LV_EVENT_DELETE, nullptr);

    title = make_label(65, 272, INK);
    timer_label = make_label(215, 220, INK);
    hint = make_label(218, 272, MUTED);
    dot = lv_obj_create(root);
    lv_obj_remove_style_all(dot);
    isolate(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(RED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, -65, 77);
    level = lv_bar_create(root);
    isolate(level);
    lv_obj_set_size(level, 120, 4);
    lv_obj_align(level, LV_ALIGN_TOP_MID, 0, 207);
    lv_bar_set_range(level, 0, 32767);
    lv_obj_set_style_bg_color(level, lv_color_hex(0xd4cfc3), LV_PART_MAIN);
    lv_obj_set_style_bg_color(level, lv_color_hex(RED), LV_PART_INDICATOR);
    voice_motion_create(level, dot);
    page_image = lv_image_create(root);
    isolate(page_image);
    lv_obj_align(page_image, LV_ALIGN_TOP_MID, 0, 96);
    make_button(previous);
    make_button(primary);
    make_button(secondary);
    make_button(talk, screen);
    lv_obj_remove_event_cb(talk.obj, button_event);
    lv_obj_add_event_cb(talk.obj, hold_event, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(talk.obj, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_size(talk.obj, 176, 52);
    lv_obj_align(talk.obj, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_add_flag(talk.obj, LV_OBJ_FLAG_HIDDEN);
}

bool voice_ui_active()
{
    return holding || (root && !lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
}
