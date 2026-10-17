#pragma once

#include "lvgl.h"
#include "voice_character.h"

class CharacterPresenter {
public:
    CharacterPresenter() = default;
    CharacterPresenter(const CharacterPresenter &) = delete;
    CharacterPresenter &operator=(const CharacterPresenter &) = delete;
    ~CharacterPresenter();
    void attach(lv_obj_t *canvas, Fb framebuffer);
    CharacterMode update(uint32_t now);
    void draw_home(CharacterDrawParams params);
    void reset() { renderer_.reset(); skin_ = -1; }
private:
    void present(PixelRect dirty);
    static void event(lv_event_t *event);
    void touch(lv_event_t *event);
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *surface_ = nullptr;
    Fb fb_{};
    CharacterRenderer renderer_;
    VoiceCharacterMotion motion_;
    CharacterMode mode_ = CharacterMode::Covered;
    int skin_ = -1;
    bool moving_ = false;
    bool unread_ = false;
    uint32_t notified_at_ = 0;
    float avatar_x_ = 0, avatar_y_ = 0, avatar_radius_ = 0;
    lv_point_t pressed_at_{};
    uint32_t pressed_time_ = 0, pressed_generation_ = 0;
    bool tracking_ = false, moved_ = false, consume_click_ = false;
};
