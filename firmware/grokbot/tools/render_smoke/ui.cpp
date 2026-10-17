#include "render_cases.h"
#include "lvgl.h"
#include "refresh_guard.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error "Display checks require assertions"
#endif
struct Surface {
    lv_display_t *display;
    lv_obj_t *canvas, *overlay;
    std::vector<uint16_t> framebuffer = std::vector<uint16_t>(320*320);
    std::vector<uint16_t> panel = std::vector<uint16_t>(360*360, 0xdead);
    std::array<uint16_t,360*40> buffer{};
    display_refresh_guard_t guard{};
    CharacterRenderer renderer;
    int fail_strip = -1, strips = 0, completed = 0;
    size_t bytes = 0;

    Surface() {
        display = lv_display_create(360,360);
        lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display,buffer.data(),nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_user_data(display,this);
        display_refresh_guard_attach(&guard,display);
        lv_display_add_event_cb(display,[](lv_event_t *e) {
            static_cast<Surface*>(lv_event_get_user_data(e))->strips=0;
        },LV_EVENT_REFR_START,this);
        lv_display_set_flush_cb(display,[](lv_display_t *d,const lv_area_t *a,uint8_t *data) {
            auto &s=*static_cast<Surface*>(lv_display_get_user_data(d));
            const bool last=lv_display_flush_is_last(d);
            const int strip=s.strips++;
            if(strip==s.fail_strip || (s.fail_strip==-2 && last)) {
                s.guard.failed=true;
                s.fail_strip=-1;
            } else {
                const size_t stride=lv_display_get_buf_active(d)->header.stride;
                for(int y=a->y1;y<=a->y2;++y)
                    std::memcpy(s.panel.data()+y*360+a->x1,data+(y-a->y1)*stride,(a->x2-a->x1+1)*2);
                s.bytes+=(a->x2-a->x1+1)*(a->y2-a->y1+1)*2;
            }
            if(last && !s.guard.failed) ++s.completed;
            lv_display_flush_ready(d);
        });
        auto *screen=lv_display_get_screen_active(display);
        lv_obj_set_style_bg_color(screen,lv_color_hex(0xece7db),0);
        lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
        canvas=lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas,framebuffer.data(),320,320,LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas,20,20);
        overlay=lv_obj_create(lv_display_get_layer_top(display));
        lv_obj_set_size(overlay,110,48);
        lv_obj_set_pos(overlay,104,106);
        lv_obj_set_style_bg_color(overlay,lv_color_hex(0x2472a3),0);
        lv_obj_set_style_bg_opa(overlay,LV_OPA_60,0);
    }
    Fb fb() { return {framebuffer.data(),320,320}; }
    void refresh() { lv_refr_now(display); }
    void draw(const BotPose &pose,float time,bool full) {
        auto p=render_case(fb(),pose,time);
        if(full) {
            auto f=fb();fb_clear(f,p.paper);character_draw(p);lv_obj_invalidate(canvas);
        } else {
            auto dirty=renderer.render(p);
            if(!dirty.empty()) {
                lv_area_t origin;lv_obj_get_coords(canvas,&origin);
                lv_area_t area={origin.x1+dirty.x1,origin.y1+dirty.y1,origin.x1+dirty.x2,origin.y1+dirty.y2};
                lv_obj_invalidate_area(canvas,&area);
            }
        }
    }
    ~Surface() { display_refresh_guard_detach(&guard);lv_display_delete(display); }
};

int main()
{
    lv_init();
    Surface reference,partial;
    size_t frames=0;
    for(int i=0;i<180;++i) {
        character_select((i/60)%3);
        BotPose pose;float t=i*.17f;
        poseFor(static_cast<BotState>((i/8)%static_cast<int>(BotState::COUNT)),t,pose);
        if(i%31==0) {
            lv_obj_set_pos(reference.canvas,10+(i%2)*13,17);
            lv_obj_set_pos(partial.canvas,10+(i%2)*13,17);
        }
        reference.draw(pose,t,true);partial.draw(pose,t,false);
        reference.refresh();partial.refresh();
        assert(reference.panel==partial.panel);++frames;
        if(i%29==0) {
            for(auto *s : {&reference,&partial}) {
                lv_obj_add_flag(s->canvas,LV_OBJ_FLAG_HIDDEN);
                s->renderer.reset();s->refresh();
            }
            assert(reference.panel==partial.panel);
            for(auto *s : {&reference,&partial}) lv_obj_remove_flag(s->canvas,LV_OBJ_FLAG_HIDDEN);
        }
    }
    BotPose pose;poseFor(BotState::Idle,1,pose);
    for(int failed_strip : {0,2,-2}) {
        reference.draw(pose,1,true);reference.refresh();
        partial.draw(pose,1,false);
        std::fill(partial.panel.begin(),partial.panel.end(),0xdead);
        lv_obj_invalidate(lv_display_get_layer_sys(partial.display));
        const int before=partial.completed;
        partial.fail_strip=failed_strip;
        partial.refresh();
        assert(partial.guard.failed && partial.completed==before);
        assert(partial.panel!=reference.panel);
        partial.refresh();
        assert(!partial.guard.failed && partial.completed==before+1);
        assert(partial.panel==reference.panel);
    }
    std::printf("PASS: %zu real-LVGL composite frames, moved canvas, translucent overlay, hidden recovery; first/middle/last strip failure restores full display\n",frames);
}
