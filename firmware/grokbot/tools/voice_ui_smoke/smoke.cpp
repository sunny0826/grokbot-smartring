#include "voice_ui.h"
#include "home_controls.h"
#include "voice.h"
#include "voice_page.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error "The behavioral smoke requires assertions; build with Debug configuration."
#endif
static VoiceSnapshot state;
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
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    for(int y=a->y1; y<=a->y2; ++y)
        for(int x=a->x1; x<=a->x2; ++x) {
            auto i=(y*360+x)*3;
            pixels[i]=p[2]; pixels[i+1]=p[1]; pixels[i+2]=p[0]; p+=3;
        }
    lv_display_flush_ready(d);
}
static lv_obj_t *find(lv_obj_t *o, const char *text) {
    if(lv_obj_check_type(o, &lv_label_class) && !std::strcmp(lv_label_get_text(o),text)) return o;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);++i) if(auto *r=find(lv_obj_get_child(o,i),text)) return r;
    return nullptr;
}
static void tick() { lv_tick_inc(110); lv_timer_handler(); }
static void phase(VoicePhase value) { state.phase=value; ++state.generation; tick(); }
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
static void screenshot(const char *path) {
    lv_tick_inc(200); lv_timer_handler(); lv_refr_now(nullptr);
    FILE *f=std::fopen(path,"wb"); assert(f); std::fprintf(f,"P6\n360 360\n255\n"); std::fwrite(pixels.data(),1,pixels.size(),f); std::fclose(f);
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
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB888);
    static unsigned char buffer[360*40*3];
    lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    auto *input=lv_indev_create();
    lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input,display);
    lv_indev_set_read_cb(input,[](lv_indev_t *, lv_indev_data_t *data) {
        data->point=pointer; data->state=pointer_state;
    });
    int pet_clicks=0;
    lv_obj_add_event_cb(lv_screen_active(),[](lv_event_t *e) {++*static_cast<int *>(lv_event_get_user_data(e));},LV_EVENT_CLICKED,&pet_clicks);
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
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
    assert(!lv_obj_has_flag(button("未读回复"),LV_OBJ_FLAG_HIDDEN));
    expect("未读回复",VoiceAction::Open); assert(pet_clicks==0);
    click(lv_screen_active()); assert(pet_clicks==1);
    state.duration_ms=12500; state.peak=16000; phase(VoicePhase::Recording);
    assert(voice_ui_active()); assert(find(lv_screen_active(),"正在录音"));
    assert(lv_obj_has_flag(button("未读回复"),LV_OBJ_FLAG_HIDDEN));
    expect("取消录音",VoiceAction::Discard);
    screenshot("recording.ppm");
    auto *cancel=button("取消录音");
    int before=actions;
    lv_obj_send_event(cancel,LV_EVENT_PRESSED,nullptr);
    ++state.generation; tick(); lv_obj_send_event(cancel,LV_EVENT_CLICKED,nullptr); assert(actions==before);
    phase(VoicePhase::Uploading);
    auto *root=lv_obj_get_child(lv_screen_active(),1);
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        auto *child=lv_obj_get_child(root,i);
        if(lv_obj_check_type(child,&lv_button_class)) assert(lv_obj_has_flag(child,LV_OBJ_FLAG_HIDDEN));
    }
    phase(VoicePhase::Failed); expect("重试",VoiceAction::Retry);
    phase(VoicePhase::UploadFailed); screenshot("failure.ppm");
    phase(VoicePhase::Waiting); assert(find(root,"等待 Grok 领取")); expect("返回",VoiceAction::Close);
    phase(VoicePhase::Claimed); assert(find(root,"Grok 已领取"));
    phase(VoicePhase::NeedsReview); assert(find(root,"任务需要核对"));
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
    assert(!lv_obj_has_flag(button("未读回复"),LV_OBJ_FLAG_HIDDEN)); expect("未读回复",VoiceAction::Open);
    auto *unread=button("未读回复"); before=actions;
    lv_obj_send_event(unread,LV_EVENT_PRESSED,nullptr);
    phase(VoicePhase::Recording); lv_obj_send_event(unread,LV_EVENT_CLICKED,nullptr); assert(actions==before);
    phase(VoicePhase::Unavailable); std::strcpy(state.error,"empty_recording"); tick(); assert(find(root,"未录到有效语音"));
    phase(VoicePhase::Idle); state.unread=false; tick(); assert(lv_obj_has_flag(button("未读回复"),LV_OBJ_FLAG_HIDDEN));
    pointer={180,300}; pointer_state=LV_INDEV_STATE_PRESSED; tick();
    const auto ends_before_delete=hold_ends.size();
    lv_obj_delete(root); assert(!voice_ui_active());
    assert(hold_ends.size()==ends_before_delete+1 && hold_ends.back()==VoiceHoldEnd::Cancel);
    pointer_state=LV_INDEV_STATE_RELEASED; tick();
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
    lv_obj_clean(lv_screen_active()); assert(!voice_ui_active());
    auto *temporary_screen = lv_obj_create(nullptr);
    voice_ui_create(temporary_screen);
    lv_obj_delete(temporary_screen); assert(!voice_ui_active());
    voice_ui_create(lv_screen_active()); assert(!voice_ui_active());
    puts("voice UI real LVGL: pointer hold/release, slide cancellation, quick tap, gesture capture, recording, inbox, paging, explicit read, cache ownership, stale clicks PASS");
}
