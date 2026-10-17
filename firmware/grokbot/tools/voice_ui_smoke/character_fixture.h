#pragma once
#include "character_presenter.h"
#include <vector>

struct CharacterFixture {
    std::vector<uint16_t> pixels = std::vector<uint16_t>(320 * 320);
    CharacterPresenter presenter;
    lv_obj_t *canvas = nullptr;
    lv_timer_t *timer = nullptr;
    CharacterMode mode = CharacterMode::Covered;

    Fb fb() { return {pixels.data(), 320, 320}; }
    void create(lv_obj_t *screen) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xece7db), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        canvas = lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas, pixels.data(), 320, 320, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(canvas);
        presenter.attach(canvas, fb());
    }
    void frame() {
        mode = presenter.update(lv_tick_get());
        if (mode == CharacterMode::Home) {
            BotPose pose; poseFor(BotState::Idle, 0, pose);
            presenter.draw_home({fb(), 160, 160, 0, 100, 100, 1, 0, 1,
                pose.gaze, &pose, 1, 100, 0, rgb565(10,10,12), rgb565(236,231,219), 1, 0});
        }
    }
    void start() {
        timer = lv_timer_create([](lv_timer_t *timer) {
            static_cast<CharacterFixture *>(lv_timer_get_user_data(timer))->frame();
        }, LV_DEF_REFR_PERIOD, this);
    }
    ~CharacterFixture() { if (timer) lv_timer_delete(timer); }
};
