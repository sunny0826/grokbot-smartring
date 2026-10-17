#include "character_presenter.h"
#include "voice_ui.h"
#include "wifi_setup_ui.h"
#include <cmath>
#ifdef MY_BOT_DISPLAY_BENCHMARK
#include "device_benchmark.h"
#endif

CharacterPresenter::~CharacterPresenter()
{
    if (canvas_) lv_obj_remove_event_cb_with_user_data(canvas_, nullptr, this);
    if (surface_) lv_obj_remove_event_cb_with_user_data(surface_, nullptr, this);
}

void CharacterPresenter::attach(lv_obj_t *canvas, Fb framebuffer)
{
    if (canvas_) lv_obj_remove_event_cb_with_user_data(canvas_, nullptr, this);
    if (surface_) lv_obj_remove_event_cb_with_user_data(surface_, nullptr, this);
    canvas_ = canvas;
    surface_ = lv_obj_get_parent(canvas);
    fb_ = framebuffer;
    mode_ = CharacterMode::Covered;
    motion_ = {};
    unread_ = false;
    tracking_ = consume_click_ = false;
    avatar_radius_ = 0;
    reset();
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(canvas_, event, LV_EVENT_ALL, this);
    lv_obj_add_event_cb(surface_, event, LV_EVENT_ALL, this);
}

void CharacterPresenter::event(lv_event_t *event)
{
    auto *self = static_cast<CharacterPresenter *>(lv_event_get_user_data(event));
    auto *target = lv_event_get_target_obj(event);
    if (target != self->canvas_ && target != self->surface_) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (target == self->surface_) self->surface_ = nullptr;
        if (target == self->canvas_) { self->canvas_ = nullptr; self->fb_ = {}; }
        self->tracking_ = self->consume_click_ = false;
        return;
    }
    self->touch(event);
}

void CharacterPresenter::touch(lv_event_t *event)
{
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        if (consume_click_) { lv_event_stop_processing(event); lv_event_stop_bubbling(event); }
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) { tracking_ = consume_click_ = false; return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    auto *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point; lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        tracking_ = consume_click_ = false;
        const float dx = point.x - avatar_x_, dy = point.y - avatar_y_;
        if (!canvas_ || mode_ != CharacterMode::Home || !unread_ || avatar_radius_ <= 0 ||
            voice_ui_active() || wifi_setup_ui_active() || dx*dx+dy*dy > avatar_radius_*avatar_radius_) return;
        const auto snapshot = voice_snapshot();
        if (snapshot.phase != VoicePhase::Idle || !snapshot.unread) return;
        pressed_at_ = point; pressed_time_ = lv_tick_get(); pressed_generation_ = snapshot.generation;
        tracking_ = true; moved_ = false;
    } else if (tracking_ && (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)) {
        const int dx = point.x - pressed_at_.x, dy = point.y - pressed_at_.y;
        moved_ = moved_ || std::abs(dx) > 40 || std::abs(dy) > 40;
        if (!moved_) {
            lv_event_stop_processing(event); lv_event_stop_bubbling(event);
            if (code == LV_EVENT_RELEASED) {
                consume_click_ = true;
                const auto snapshot = voice_snapshot();
                if (lv_tick_elaps(pressed_time_) < 400 && mode_ == CharacterMode::Home &&
                    !voice_ui_active() && !wifi_setup_ui_active() && snapshot.phase == VoicePhase::Idle &&
                    snapshot.unread && snapshot.generation == pressed_generation_)
                    voice_action(VoiceAction::Open, pressed_generation_);
            }
        }
        if (code == LV_EVENT_RELEASED) tracking_ = false;
    }
}

void CharacterPresenter::present(PixelRect dirty)
{
    if (!dirty.empty()) {
        lv_area_t origin;
        lv_obj_get_coords(canvas_, &origin);
        const lv_area_t area = {origin.x1 + dirty.x1, origin.y1 + dirty.y1,
                               origin.x1 + dirty.x2, origin.y1 + dirty.y2};
        lv_obj_invalidate_area(canvas_, &area);
    }
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    skin_ = character_index();
}

CharacterMode CharacterPresenter::update(uint32_t now)
{
    if (!canvas_) return CharacterMode::Covered;
    const auto view = voice_ui_update(now, wifi_setup_ui_active());
    if (view.unread && !unread_) notified_at_ = now;
    unread_ = view.unread;
    const bool changed = motion_.observe(view, now);
    const auto previous = mode_;
    mode_ = view.mode;
    if (mode_ != CharacterMode::Home) tracking_ = consume_click_ = false;
    if (mode_ == CharacterMode::Covered) {
        if (previous != mode_) {
            lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
            reset();
        }
    } else if (mode_ == CharacterMode::Voice) {
        const bool moving = motion_.moving(now);
        if (changed || previous != mode_ || skin_ != character_index() || moving || moving_) {
            if (previous != mode_ || skin_ != character_index()) renderer_.reset();
            present(renderer_.render(motion_.sample(fb_, now)));
        }
        moving_ = moving;
    }
#ifdef MY_BOT_DISPLAY_BENCHMARK
    if (mode_ != CharacterMode::Home) display_benchmark_pause();
#endif
    return mode_;
}

void CharacterPresenter::draw_home(CharacterDrawParams params)
{
    if (!canvas_ || mode_ != CharacterMode::Home) return;
    params.fb = fb_;
    lv_obj_update_layout(canvas_);
    lv_area_t origin; lv_obj_get_coords(canvas_, &origin);
    avatar_x_ = origin.x1 + params.rcx;
    avatar_y_ = origin.y1 + params.rcy;
    avatar_radius_ = fmaxf(22.f, fmaxf(params.erx, params.ery)) + 12.f;
    BotPose home_pose = *params.pose;
    if (unread_) {
        BotPose notification;
        poseFor(BotState::Notify, (lv_tick_get() - notified_at_) / 1000.f, notification);
        home_pose.notifX = notification.notifX * home_pose.bodyR;
        home_pose.notifY = notification.notifY * home_pose.bodyR;
        home_pose.notifR = notification.notifR * home_pose.bodyR;
        home_pose.notifA = notification.notifA;
        params.pose = &home_pose;
    }
    if (skin_ != character_index()) renderer_.reset();
    PixelRect dirty;
#ifdef MY_BOT_DISPLAY_BENCHMARK
    if (!display_benchmark_render(params, renderer_, dirty))
#endif
        dirty = renderer_.render(params);
    present(dirty);
    moving_ = false;
}
