#include "voice_ui.h"
#include "home_controls.h"
#include "voice.h"
#include "voice_page.h"
#include "character_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <functional>
#include <filesystem>
#include <algorithm>

#ifdef NDEBUG
#error "The behavioral smoke requires assertions; build with Debug configuration."
#endif
static VoiceSnapshot state;
static bool wifi_active;
bool wifi_setup_ui_active() { return wifi_active; }
static int actions;
static VoiceAction action;
static uint32_t generation;
static uint32_t hold_token;
static int hold_begins;
static std::vector<VoiceHoldEnd> hold_ends;
static lv_point_t pointer = {180, 300};
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;
uint32_t voice_hold_begin() { ++hold_begins; return ++hold_token; }
void voice_hold_end(uint32_t id, VoiceHoldEnd result) { assert(id == hold_token); hold_ends.push_back(result); }
static std::vector<uint8_t> page(VOICE_PAGE_BYTES, 0x88);
VoiceSnapshot voice_snapshot() { return state; }
bool voice_action(VoiceAction a, uint32_t g) { ++actions; action=a; generation=g; return true; }
bool voice_copy_page(uint32_t g, uint8_t *out, size_t n) {
    if(g!=state.generation || n!=page.size()) return false;
    memcpy(out,page.data(),n); return true;
}
void voice_start() {}
static std::vector<unsigned char> pixels(360*360*3);
static unsigned completed_frames;
static std::function<void()> frame_probe;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    const size_t stride=lv_display_get_buf_active(d)->header.stride;
    for(int y=a->y1; y<=a->y2; ++y)
        for(int x=a->x1; x<=a->x2; ++x) {
            auto i=(y*360+x)*3;
            const auto *source=p+(y-a->y1)*stride+(x-a->x1)*2;
            const uint16_t color=source[0] | uint16_t(source[1])<<8;
            const uint8_t r=color>>11, g=(color>>5)&63, b=color&31;
            pixels[i]=(r<<3)|(r>>2); pixels[i+1]=(g<<2)|(g>>4); pixels[i+2]=(b<<3)|(b>>2);
        }
    if(lv_display_flush_is_last(d)) { ++completed_frames; if(frame_probe) frame_probe(); }
    lv_display_flush_ready(d);
}
static lv_obj_t *find(lv_obj_t *o, const char *text) {
    if(lv_obj_check_type(o, &lv_label_class) && !std::strcmp(lv_label_get_text(o),text)) return o;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);++i) if(auto *r=find(lv_obj_get_child(o,i),text)) return r;
    return nullptr;
}
static lv_obj_t *find_type(lv_obj_t *object, const lv_obj_class_t *type) {
    if(lv_obj_check_type(object,type)) return object;
    for(uint32_t i=0;i<lv_obj_get_child_count(object);++i)
        if(auto *found=find_type(lv_obj_get_child(object,i),type)) return found;
    return nullptr;
}
static void tick() { lv_tick_inc(110); lv_timer_handler(); }
static void phase(VoicePhase value) { state.phase=value; ++state.generation; tick(); }
static void tap(int x,int y) {
    pointer={x,y}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    pointer_state=LV_INDEV_STATE_RELEASED; tick();
}
static lv_obj_t *button(const char *text) { auto *label=find(lv_screen_active(),text); assert(label); return lv_obj_get_parent(label); }
static void click(lv_obj_t *obj) {
    lv_obj_send_event(obj, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(obj, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
}
static void expect(const char *label, VoiceAction expected) {
    const int before=actions; click(button(label)); assert(actions==before+1 && action==expected && generation==state.generation);
}
static void capture(const char *path) {
    FILE *f=std::fopen(path,"wb"); assert(f); std::fprintf(f,"P6\n360 360\n255\n"); std::fwrite(pixels.data(),1,pixels.size(),f); std::fclose(f);
}
static void screenshot(const char *path) {
    lv_tick_inc(200); lv_timer_handler(); lv_refr_now(nullptr); capture(path);
}

static void notification_preserves_pose(CharacterFixture &character) {
    for(auto animation:{BotState::Thinking,BotState::Dizzy,BotState::Orbit}) {
        BotPose pose; poseFor(animation,1,pose);
        const BotPose original=pose;
        const float radius=100*pose.bodyR;
        CharacterDrawParams params{character.fb(),160+100*pose.bodyCx,160+100*pose.bodyCy,
            0,radius,radius,1,0,1,pose.gaze,&pose,1,100,1,
            rgb565(10,10,12),rgb565(236,231,219),1,0};
        state.unread=false; ++state.generation; tick();
        character.presenter.draw_home(params);
        const auto before=character.pixels;
        state.unread=true; ++state.generation; tick();
        character.presenter.draw_home(params);
        assert(std::memcmp(&pose,&original,sizeof(pose))==0);
        unsigned changed=0;
        for(int y=0;y<320;++y) for(int x=0;x<320;++x) {
            if(character.pixels[y*320+x]==before[y*320+x]) continue;
            ++changed;
            assert(x>=params.rcx+.45f*radius-2 && x<=params.rcx+1.05f*radius+2);
            assert(y>=params.rcy-radius-2 && y<=params.rcy-.35f*radius+2);
        }
        assert(changed>0 && changed<1800);
    }
    state.unread=false; ++state.generation; tick();
}

static unsigned visible_labels(lv_obj_t *object) {
    if(lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)) return 0;
    unsigned count=lv_obj_check_type(object,&lv_label_class) && lv_label_get_text(object)[0];
    for(uint32_t i=0;i<lv_obj_get_child_count(object);++i) count+=visible_labels(lv_obj_get_child(object,i));
    return count;
}

static void avatar_entry_checks(CharacterFixture &character) {
    struct Events { unsigned pressing=0, released=0; } events;
    auto probe=[](lv_event_t *event) {
        auto &counts=*static_cast<Events *>(lv_event_get_user_data(event));
        counts.pressing+=lv_event_get_code(event)==LV_EVENT_PRESSING;
        counts.released+=lv_event_get_code(event)==LV_EVENT_RELEASED;
    };
    auto *screen=lv_screen_active();
    lv_obj_add_event_cb(screen,probe,LV_EVENT_ALL,&events);
    lv_obj_add_event_cb(character.canvas,probe,LV_EVENT_ALL,&events);
    state.unread=true; phase(VoicePhase::Idle);
    for(int skin=0;skin<character_count();++skin) for(bool clickable:{false,true}) {
        character_select(skin); tick();
        lv_obj_set_flag(character.canvas,LV_OBJ_FLAG_CLICKABLE,clickable);
        int before=actions; const auto released=events.released; tap(180,180);
        assert(actions==before+1 && action==VoiceAction::Open && events.released==released);
        before=actions; tap(25,180); assert(actions==before && events.released>released);
        pointer={180,180}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
        const auto pressing=events.pressing; const auto drag_released=events.released;
        pointer={255,180}; tick(); pointer_state=LV_INDEV_STATE_RELEASED; tick();
        assert(actions==before && events.pressing>pressing && events.released>drag_released);
        pointer={180,180}; pointer_state=LV_INDEV_STATE_PRESSED;
        for(int i=0;i<6;++i) tick();
        pointer_state=LV_INDEV_STATE_RELEASED; tick(); assert(actions==before);
        pointer_state=LV_INDEV_STATE_PRESSED; tick(); ++state.generation;
        pointer_state=LV_INDEV_STATE_RELEASED; tick(); assert(actions==before);
        pointer_state=LV_INDEV_STATE_PRESSED; tick(); state.unread=false;
        pointer_state=LV_INDEV_STATE_RELEASED; tick(); assert(actions==before);
        state.unread=true; ++state.generation; tick();
        pointer_state=LV_INDEV_STATE_PRESSED; tick(); wifi_active=true;
        pointer_state=LV_INDEV_STATE_RELEASED; tick(); assert(actions==before);
        wifi_active=false; tick();
    }
    lv_obj_remove_flag(character.canvas,LV_OBJ_FLAG_CLICKABLE);
    lv_timer_pause(character.timer);
    BotPose pose; poseFor(BotState::Idle,0,pose);
    CharacterDrawParams params{character.fb(),100,110,0,50,50,1,0,1,pose.gaze,&pose,
        1,50,0,rgb565(10,10,12),rgb565(236,231,219),1,0};
    character.presenter.draw_home(params); lv_refr_now(nullptr);
    int before=actions; tap(180,180); assert(actions==before);
    tap(120,130); assert(actions==before+1 && action==VoiceAction::Open);
    poseFor(BotState::Thinking,1,pose);
    params.rcx=200; params.rcy=100; params.erx=params.ery=8; params.scale=44;
    character.presenter.draw_home(params); lv_refr_now(nullptr);
    before=actions; tap(240,120); assert(actions==before+1);
    lv_timer_resume(character.timer);
    character_select(0);
    state.unread=false; ++state.generation; tick(); before=actions; tap(180,180); assert(actions==before);
    lv_obj_remove_event_cb_with_user_data(screen,probe,&events);
    lv_obj_remove_event_cb_with_user_data(character.canvas,probe,&events);
    for(auto value:{VoicePhase::Uploading,VoicePhase::Stored,VoicePhase::Transcribing,
                   VoicePhase::Waiting,VoicePhase::Claimed,VoicePhase::LoadingPage}) {
        phase(value);
        const bool returns=value==VoicePhase::Stored || value==VoicePhase::Transcribing ||
                           value==VoicePhase::Waiting || value==VoicePhase::Claimed;
        assert(visible_labels(screen)==(returns ? 2u : 1u));
    }
    phase(VoicePhase::Idle);
    puts("Avatar entry PASS: no inbox button, single tap opens once, drag reaches home handlers, long/stale/covered taps ignored, moving hit area, concise progress text");
}

static void fusion_checks(CharacterFixture &character) {
    state.hold_id=0; state.unread=false; state.error[0]=0;
    for(int skin=0;skin<character_count();++skin) {
        character_select(skin); phase(VoicePhase::Idle); lv_refr_now(nullptr);
        unsigned observed=0;
        frame_probe=[&] {
            auto *label=find(lv_screen_active(),"准备录音");
            if(label && !lv_obj_has_flag(lv_obj_get_parent(label),LV_OBJ_FLAG_HIDDEN)) {
                const int i=(180*360+100)*3;
                assert(pixels[i]>220 && pixels[i+1]>220 && pixels[i+2]>210);
                ++observed;
            }
        };
        phase(VoicePhase::Preparing); lv_refr_now(nullptr);
        assert(observed); frame_probe={};
        phase(VoicePhase::Claimed); tick();
        const auto before_cover=character.pixels;
        wifi_active=true; tick();
        assert(lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        phase(VoicePhase::Recording); state.peak=30000; tick();
        phase(VoicePhase::Waiting); tick();
        assert(character.pixels==before_cover);
        wifi_active=false; tick(); lv_refr_now(nullptr);
        assert(!lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        assert(character.pixels!=before_cover);
        for(auto error:{VoicePhase::Ready,VoicePhase::UploadFailed,VoicePhase::Failed,
                        VoicePhase::NeedsReview,VoicePhase::ReplyFailed,VoicePhase::Unavailable}) {
            phase(error); lv_refr_now(nullptr);
            const auto stable=character.pixels; const auto flushes=completed_frames;
            for(int i=0;i<8;++i)tick();
            if (character.pixels!=stable || completed_frames!=flushes)
                fprintf(stderr,"static phase %d skin %d: pixels_changed=%d refreshes=%u\n",int(error),skin,character.pixels!=stable,completed_frames-flushes);
            assert(character.pixels==stable && completed_frames==flushes);
        }
        state.page_count=2; state.page_index=0; phase(VoicePhase::ViewingReply);
        const auto reading=character.pixels;
        for(int i=0;i<5;++i)tick();
        assert(character.pixels==reading && lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        phase(VoicePhase::Preparing); lv_refr_now(nullptr);
        assert(!lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        phase(VoicePhase::Idle); lv_refr_now(nullptr);
        assert(character.mode==CharacterMode::Home && !voice_ui_active());
        const auto blue_pixels=[&] { return std::count(character.pixels.begin(),character.pixels.end(),NOTIF_BLUE565); };
        assert(blue_pixels()==0);
        state.unread=true; ++state.generation; tick();
        assert(character.mode==CharacterMode::Home && !voice_ui_active() && blue_pixels()>100);
        for(int i=0;i<5;++i) tick();
        const auto settled_notification=character.pixels;
        for(int i=0;i<10;++i) { ++state.generation; tick(); assert(character.pixels==settled_notification); }
        wifi_active=true; tick();
        const auto unread_covered=character.pixels;
        for(int i=0;i<10;++i) tick();
        assert(character.pixels==unread_covered);
        wifi_active=false; tick();
        assert(character.pixels==settled_notification && blue_pixels()>100);
        state.unread=false; ++state.generation; tick();
        assert(blue_pixels()==0 && character.mode==CharacterMode::Home);
        notification_preserves_pose(character);
    }
    character_select(0);
    puts("Fused UI PASS: first visible frame clears Home, three skins, covered canvas unchanged, static errors cause zero refreshes, reading/home restoration, unread badge persists without restarting and clears on read");
}

static void fusion_previews() {
    const VoicePhase phases[]={VoicePhase::Recording,VoicePhase::Uploading,VoicePhase::Waiting,
        VoicePhase::Claimed,VoicePhase::Replied,VoicePhase::NeedsReview,VoicePhase::LoadingPage};
    const char *names[]={"recording","uploading","waiting","claimed","replied","review","loading"};
    std::filesystem::create_directory("fusion-frames");
    state.duration_ms=1200; state.hold_id=0; state.error[0]=0;
    for(int skin=0;skin<character_count();++skin) {
        character_select(skin);
        phase(VoicePhase::Idle); state.unread=true; ++state.generation; tick();
        char unread_path[128]; snprintf(unread_path,sizeof(unread_path),"fusion-frames/%s-unread.ppm",character_name(skin));
        screenshot(unread_path); state.unread=false; ++state.generation; tick();
        for(int stage=0;stage<7;++stage) {
            phase(phases[stage]);
            for(int frame=0;frame<24;++frame) {
                state.peak=stage==0 ? uint16_t((frame%12)*2400) : 0;
                lv_tick_inc(100); lv_timer_handler(); lv_refr_now(nullptr);
                char name[160]; snprintf(name,sizeof(name),"fusion-frames/%s-%s-%02d.ppm",character_name(skin),names[stage],frame);
                capture(name);
            }
        }
    }
    character_select(0); state.peak=0; phase(VoicePhase::Idle);
}
static void fixture(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f);
    std::vector<uint8_t> bytes(VOICE_PAGE_BYTES+VOICE_PAGE_HEADER);
    assert(fread(bytes.data(),1,bytes.size(),f)==bytes.size()); assert(fgetc(f)==EOF); fclose(f);
    assert(voice_page_valid(bytes.data(),bytes.size(),voice_page_le(bytes.data()+8,2),voice_page_le(bytes.data()+10,2)));
    memcpy(page.data(),bytes.data()+VOICE_PAGE_HEADER,page.size());
}
int main(int argc, char **argv) {
    if(argc>1) fixture(argv[1]);
    lv_init();
    auto *display=lv_display_create(360,360);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    static unsigned char buffer[360*40*2];
    lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    CharacterFixture character;
    character.create(lv_screen_active());
    auto *input=lv_indev_create();
    lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input,display);
    lv_indev_set_read_cb(input,[](lv_indev_t *, lv_indev_data_t *data) {
        data->point=pointer; data->state=pointer_state;
    });
    int pet_clicks=0;
    lv_obj_add_event_cb(lv_screen_active(),[](lv_event_t *e) {++*static_cast<int *>(lv_event_get_user_data(e));},LV_EVENT_CLICKED,&pet_clicks);
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
    character.start();
    home_controls_attach(lv_screen_active());
    screenshot("home.ppm");
    auto *hold_button=button("按住说话");
    assert(lv_obj_has_flag(hold_button,LV_OBJ_FLAG_HIDDEN));
    pointer_state=LV_INDEV_STATE_PRESSED; tick();
    pointer_state=LV_INDEV_STATE_RELEASED; tick(); tick();
    assert(hold_begins==0 && !lv_obj_has_flag(hold_button,LV_OBJ_FLAG_HIDDEN));
    pet_clicks=0;
    pointer_state=LV_INDEV_STATE_PRESSED; tick();
    assert(hold_begins==1 && voice_ui_active());
    state.hold_id=hold_token; phase(VoicePhase::Preparing);
    assert(!lv_obj_has_flag(hold_button,LV_OBJ_FLAG_HIDDEN));
    state.duration_ms=1500; state.peak=16000; phase(VoicePhase::Recording);
    for(int i=0;i<100;++i) tick();
    assert(!lv_obj_has_flag(hold_button,LV_OBJ_FLAG_HIDDEN) && hold_ends.empty());
    assert(button("松开发送")==hold_button);
    screenshot("touch-recording.ppm");
    pointer_state=LV_INDEV_STATE_RELEASED; tick();
    assert(hold_ends.size()==1 && hold_ends.back()==VoiceHoldEnd::Send && pet_clicks==0);
    state.hold_id=0; phase(VoicePhase::Idle);
    pointer_state=LV_INDEV_STATE_PRESSED; tick();
    state.hold_id=hold_token; phase(VoicePhase::Recording);
    pointer={5,180}; tick();
    assert(hold_ends.size()==2 && hold_ends.back()==VoiceHoldEnd::Cancel);
    state.hold_id=0; phase(VoicePhase::Idle);
    assert(voice_ui_active() && find(lv_screen_active(),"已取消"));
    pointer_state=LV_INDEV_STATE_RELEASED; tick(); tick();
    assert(hold_ends.size()==2 && pet_clicks==0 && !voice_ui_active());
    pointer={180,300}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    pointer_state=LV_INDEV_STATE_RELEASED; tick();
    assert(hold_ends.size()==3 && hold_ends.back()==VoiceHoldEnd::Cancel);
    assert(pet_clicks==0);
    state.unread=true; ++state.generation; tick();
    assert(!find(lv_screen_active(),"未读回复"));
    int before_open=actions; tap(180,180);
    assert(actions==before_open+1 && action==VoiceAction::Open && generation==state.generation && pet_clicks==0);
    tap(25,180); assert(pet_clicks==1);
    state.duration_ms=12500; state.peak=16000; phase(VoicePhase::Recording);
    assert(voice_ui_active()); assert(find(lv_screen_active(),"正在录音"));
    auto *meter=find_type(lv_screen_active(),&lv_bar_class); assert(meter);
    state.peak=0;
    for(int i=0;i<12;++i) tick();
    assert(lv_bar_get_value(meter)<100);
    state.peak=24000; lv_tick_inc(33); lv_timer_handler();
    const int attack=lv_bar_get_value(meter); assert(attack>0 && attack<24000);
    for(int i=0;i<4;++i) tick();
    assert(lv_bar_get_value(meter)>23000);
    state.peak=0; lv_tick_inc(33); lv_timer_handler();
    assert(lv_bar_get_value(meter)>10000 && lv_bar_get_value(meter)<24000);
    state.peak=16000;

    assert(!find(lv_screen_active(),"未读回复"));
    expect("取消录音",VoiceAction::Discard);
    screenshot("recording.ppm");
    auto *cancel=button("取消录音");
    int before=actions;
    lv_obj_send_event(cancel,LV_EVENT_PRESSED,nullptr);
    ++state.generation; tick(); lv_obj_send_event(cancel,LV_EVENT_CLICKED,nullptr); assert(actions==before);
    phase(VoicePhase::Uploading);
    auto *root=lv_obj_get_parent(find(lv_screen_active(),"正在上传"));
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        auto *child=lv_obj_get_child(root,i);
        if(lv_obj_check_type(child,&lv_button_class)) assert(lv_obj_has_flag(child,LV_OBJ_FLAG_HIDDEN));
    }
    phase(VoicePhase::Failed); expect("重试",VoiceAction::Retry);
    phase(VoicePhase::UploadFailed); screenshot("failure.ppm");
    phase(VoicePhase::Waiting); assert(find(root,"等待接单")); expect("返回",VoiceAction::Close);
    assert(!find(root,"在 Bot 中说\n处理圆屏语音"));
    screenshot("waiting-first.ppm"); auto waiting_first=pixels;
    for(int i=0;i<4;++i) tick();
    screenshot("waiting-second.ppm"); assert(pixels!=waiting_first);
    phase(VoicePhase::Claimed); assert(find(root,"正在处理"));
    before=actions; phase(VoicePhase::Replied);
    assert(actions==before+1 && action==VoiceAction::Open && generation==state.generation);
    tick(); assert(actions==before+1);
    phase(VoicePhase::Claimed); wifi_active=true; before=actions;
    phase(VoicePhase::Replied); assert(actions==before);
    wifi_active=false; tick(); assert(actions==before);
    phase(VoicePhase::Idle); before=actions; phase(VoicePhase::Replied);
    assert(actions==before);
    phase(VoicePhase::NeedsReview); assert(find(root,"任务需要核对"));
    screenshot("review-static.ppm"); auto review_pixels=pixels;
    for(int i=0;i<4;++i) tick();
    lv_refr_now(nullptr); assert(pixels==review_pixels);
    phase(VoicePhase::Replied); expect("查看",VoiceAction::Open);
    state.page_count=2; state.page_index=0; phase(VoicePhase::ViewingReply);
    assert(find(root,"回复 1 / 2"));
    screenshot("reply-first.ppm");
    expect("下页",VoiceAction::Next);
    auto *next=button("下页"); before=actions;
    lv_obj_send_event(next,LV_EVENT_PRESSED,nullptr);
    state.page_index=1; ++state.generation; tick();
    lv_obj_send_event(next,LV_EVENT_CLICKED,nullptr); assert(actions==before);
    expect("上页",VoiceAction::Previous);
    expect("稍后",VoiceAction::Close);
    expect("已读",VoiceAction::MarkRead);
    screenshot("reply-last.ppm");
    // UI owns its copy: changing actor bytes must not alter the displayed image.
    lv_refr_now(nullptr); auto saved=pixels; memset(page.data(),0,page.size());
    lv_obj_invalidate(root); lv_refr_now(nullptr); assert(pixels==saved);
    phase(VoicePhase::Idle); assert(!voice_ui_active());
    assert(!find(lv_screen_active(),"未读回复")); before=actions; tap(180,180);
    assert(actions==before+1 && action==VoiceAction::Open);
    before=actions; pointer={180,180}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    phase(VoicePhase::Recording); pointer_state=LV_INDEV_STATE_RELEASED; tick(); assert(actions==before);
    phase(VoicePhase::Unavailable); std::strcpy(state.error,"empty_recording"); tick(); assert(find(root,"未录到有效语音"));
    phase(VoicePhase::Idle); state.unread=false; tick(); assert(!find(lv_screen_active(),"未读回复"));
    fusion_checks(character);
    avatar_entry_checks(character);
    fusion_previews();
    pointer={180,180}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    pointer_state=LV_INDEV_STATE_RELEASED; tick(); tick();
    assert(!lv_obj_has_flag(hold_button,LV_OBJ_FLAG_HIDDEN));
    const auto begins_before_delete=hold_begins;
    pointer={180,300}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    assert(hold_begins==begins_before_delete+1);
    const auto ends_before_delete=hold_ends.size();
    lv_obj_delete(root); assert(!voice_ui_active());
    assert(hold_ends.size()==ends_before_delete+1 && hold_ends.back()==VoiceHoldEnd::Cancel);
    pointer_state=LV_INDEV_STATE_RELEASED; tick();
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
    lv_obj_clean(lv_screen_active()); assert(!voice_ui_active());
    const auto deleted_pixels=character.pixels;
    tick(); lv_refr_now(nullptr);
    assert(character.mode==CharacterMode::Covered && character.pixels==deleted_pixels);
    auto *temporary_screen = lv_obj_create(nullptr);
    voice_ui_create(temporary_screen);
    lv_obj_delete(temporary_screen); assert(!voice_ui_active());
    character.create(lv_screen_active());
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
    tick(); lv_refr_now(nullptr);
    assert(character.mode==CharacterMode::Home && !lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
    puts("voice UI real LVGL: pointer hold/release, slide cancellation, quick tap, gesture capture, recording, inbox, paging, explicit read, cache ownership, stale clicks PASS");
}
