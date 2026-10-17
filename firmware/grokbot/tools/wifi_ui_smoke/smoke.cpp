#include "wifi_setup_ui.h"
#include "wifi_setup.h"
#include "wifi_setup_keyboard.h"
#include "status_bar.h"
#include "home_controls.h"
#include "voice_ui.h"
#include "voice.h"
#include "character_fixture.h"
#include "lvgl.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef NDEBUG
#error "Behavior checks require assertions"
#endif
static VoiceSnapshot voice_state;
static unsigned voice_actions, voice_holds;
VoiceSnapshot voice_snapshot() { return voice_state; }
bool voice_action(VoiceAction, uint32_t) { ++voice_actions; return true; }
uint32_t voice_hold_begin() { return ++voice_holds; }
void voice_hold_end(uint32_t, VoiceHoldEnd) {}
bool voice_copy_page(uint32_t, uint8_t *, size_t) { return false; }
static WifiSetupSnapshot state;
static unsigned joins, scans, closes, cancels;
static bool busy, busy_failed, defer_scan;
static std::string submitted;
static unsigned submitted_index;
WifiSetupSnapshot wifi_setup_snapshot() { return state; }
uint32_t wifi_setup_open(WifiSetupMethod method) {
    if (busy) return 0;
    ++state.operation; ++state.revision;
    state.method=method; state.phone_phase=method==WifiSetupMethod::Phone ? WifiPhonePhase::Starting : WifiPhonePhase::Off;
    state.phase = busy_failed ? WifiSetupPhase::Failed : WifiSetupPhase::Preparing;
    state.error = busy_failed ? WifiSetupError::VoiceBusy : WifiSetupError::None;
    return state.operation;
}
WifiPhoneDisplay wifi_setup_phone_display(uint32_t op) {
    WifiPhoneDisplay result;
    if(op!=state.operation) return result;
    result.phase=state.phone_phase; result.remaining_seconds=state.remaining_seconds;
    strcpy(result.ssid,"MyBot-AB12"); strcpy(result.password,"aB12cD34eF56"); return result;
}
bool wifi_setup_scan(uint32_t op) {
    if (op != state.operation) return false;
    ++scans; if (defer_scan) return true; ++state.revision; ++state.scan_revision; state.phase = WifiSetupPhase::Scanning; return true;
}
bool wifi_setup_connect(uint32_t op, uint32_t revision, uint8_t index, const char *password) {
    if (op != state.operation || revision != state.scan_revision || index >= state.count) return false;
    ++joins; submitted=password; submitted_index=index; ++state.revision; state.phase=WifiSetupPhase::Connecting; return true;
}
void wifi_setup_cancel(uint32_t op) { assert(op==state.operation); ++cancels; ++state.revision; state.phase=WifiSetupPhase::Restoring; }
void wifi_setup_close(uint32_t op) { assert(op==state.operation); ++closes; ++state.revision; state.phase=WifiSetupPhase::Restoring; }
static lv_point_t pointer={180,180};
static lv_indev_state_t pressed=LV_INDEV_STATE_RELEASED;
static std::vector<unsigned char> pixels(360*360*3);
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *p) {
    for(int y=area->y1;y<=area->y2;++y) for(int x=area->x1;x<=area->x2;++x) {
        auto i=(y*360+x)*3; pixels[i]=p[2]; pixels[i+1]=p[1]; pixels[i+2]=p[0]; p+=3;
    }
    lv_display_flush_ready(display);
}
static void tick() { lv_tick_inc(110); lv_timer_handler(); }
static void phase(WifiSetupPhase value) { state.phase=value; ++state.revision; tick(); }
static lv_obj_t *find(lv_obj_t *obj,const char *text) {
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if(lv_obj_check_type(obj,&lv_label_class) && !strcmp(lv_label_get_text(obj),text)) return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i) if(auto *r=find(lv_obj_get_child(obj,i),text)) return r;
    return nullptr;
}
static lv_obj_t *button(const char *text) {
    auto *label=find(lv_layer_top(),text);
    if(!label) { fprintf(stderr,"Missing label: %s\n",text); abort(); }
    return lv_obj_get_parent(label);
}
static void tap(int x,int y) { pointer={x,y}; pressed=LV_INDEV_STATE_PRESSED; tick(); pressed=LV_INDEV_STATE_RELEASED; tick(); }
static void click(const char *text) {
    lv_obj_update_layout(lv_layer_top()); lv_area_t a; lv_obj_get_coords(button(text),&a); tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);
}
static void bounds(lv_obj_t *obj) {
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)) return;
    if(lv_obj_check_type(obj,&lv_button_class)) {
        lv_area_t a; lv_obj_get_coords(obj,&a);
        assert(a.x2-a.x1+1>=44 && a.y2-a.y1+1>=44);
        for(int x:{a.x1,a.x2}) for(int y:{a.y1,a.y2}) {
            if((x-180)*(x-180)+(y-180)*(y-180)>180*180) { fprintf(stderr,"Out of circle: %d,%d\n",x,y); abort(); }
        }
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i) bounds(lv_obj_get_child(obj,i));
}
static void shot(const char *name) {
    tick(); lv_obj_update_layout(lv_layer_top()); bounds(lv_layer_top()); lv_refr_now(nullptr);
    auto masked=pixels;
    for(int y=0;y<360;++y) for(int x=0;x<360;++x) if((x-180)*(x-180)+(y-180)*(y-180)>180*180)
        memset(masked.data()+(y*360+x)*3,0,3);
    FILE *f=fopen(name,"wb"); assert(f); fprintf(f,"P6\n360 360\n255\n"); fwrite(masked.data(),1,masked.size(),f); fclose(f);
}
static void qr_shot(const char *name) {
    FILE *f=fopen(name,"wb"); assert(f); fprintf(f,"P6\n184 184\n255\n");
    for(int y=67;y<251;++y) fwrite(pixels.data()+(y*360+88)*3,1,184*3,f);
    fclose(f);
}
static void aps(unsigned count=20) {
    state.count=count; ++state.scan_revision;
    for(unsigned i=0;i<count;++i) {
        snprintf(state.aps[i].ssid,sizeof(state.aps[i].ssid),"Home Wi-Fi %02u",i+1);
        state.aps[i].rssi=-35-i*2; state.aps[i].security=WifiSetupSecurity::Personal;
    }
    if(count) strcpy(state.aps[0].ssid,"中国");
    if(count>1) strcpy(state.aps[1].ssid,"0123456789abcdefghijklmnopqrstuv");
    if(count>2) { strcpy(state.aps[2].ssid,"\xF0\x9F\x98\x80\xFF"); state.aps[2].security=WifiSetupSecurity::Open; }
    if(count>3) state.aps[3].security=WifiSetupSecurity::Unsupported;
    if(count>19) { memset(state.aps[19].ssid,0xff,32); state.aps[19].ssid[32]=0; }
    phase(WifiSetupPhase::Choosing);
}
static void select_row(unsigned row) { tap(180,116+58*row); }
static void edit_first() { select_row(0); click("密码"); }
static void clear(unsigned count) { for(unsigned i=0;i<count;++i) click("删除"); }
int main() {
    lv_init(); auto *display=lv_display_create(360,360);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB888);
    static unsigned char buffer[360*40*3];
    lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    CharacterFixture character;
    character.create(lv_screen_active());
    auto *input=lv_indev_create(); lv_indev_set_type(input,LV_INDEV_TYPE_POINTER); lv_indev_set_display(input,display);
    lv_indev_set_read_cb(input,[](lv_indev_t *,lv_indev_data_t *data){data->point=pointer;data->state=pressed;});
    unsigned underlying=0;
    auto *voice=lv_button_create(lv_screen_active()); lv_obj_set_size(voice,360,360);
    lv_obj_set_style_bg_opa(voice, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(voice, 0, 0);
    lv_obj_set_style_shadow_width(voice, 0, 0);
    lv_obj_add_event_cb(voice,[](lv_event_t *e){++*static_cast<unsigned *>(lv_event_get_user_data(e));},LV_EVENT_CLICKED,&underlying);
    home_controls_attach(voice);
    voice_state.phase=VoicePhase::Idle; voice_state.generation=1;
    status_bar_create(lv_screen_active(),true); voice_ui_create(lv_screen_active()); wifi_setup_ui_create();
    character.start(); tick();
    assert(!find(lv_layer_top(),LV_SYMBOL_WIFI) && !find(lv_screen_active(),"按住说话"));
    shot("home-hidden.ppm");
    tap(134,38); tick();
    assert(!wifi_setup_ui_active() && voice_holds==0);
    assert(find(lv_layer_top(),LV_SYMBOL_WIFI) && find(lv_screen_active(),"按住说话"));
    shot("home-revealed.ppm");
    for(int i=0;i<80;++i) tick();
    assert(find(lv_layer_top(),LV_SYMBOL_WIFI));
    tap(230,38);
    for(int i=0;i<80;++i) tick();
    assert(find(lv_layer_top(),LV_SYMBOL_WIFI));
    for(int i=0;i<12;++i) tick();
    assert(!find(lv_layer_top(),LV_SYMBOL_WIFI) && !find(lv_screen_active(),"按住说话"));
    shot("home-timeout.ppm");
    pointer={100,180}; pressed=LV_INDEV_STATE_PRESSED; tick(); pointer={200,180}; tick();
    pressed=LV_INDEV_STATE_RELEASED; tick(); tick();
    assert(!find(lv_layer_top(),LV_SYMBOL_WIFI));
    pressed=LV_INDEV_STATE_PRESSED; for(int i=0;i<6;++i) tick();
    pressed=LV_INDEV_STATE_RELEASED; tick(); tick();
    assert(!find(lv_layer_top(),LV_SYMBOL_WIFI));
    tap(180,180); tick(); assert(find(lv_layer_top(),LV_SYMBOL_WIFI));
    underlying=0;
    voice_state.phase=VoicePhase::UploadFailed; ++voice_state.generation; tick();
    lv_mem_monitor_t memory = {}; lv_mem_monitor(&memory);
    printf("Combined real voice/Wi-Fi UI LVGL live heap: %zu bytes\n", memory.total_size-memory.free_size);
    shot("entry.ppm");
    busy=true; tap(134,38); assert(wifi_setup_ui_active()); shot("method.ppm"); click("圆屏输入"); shot("busy.ppm"); click("返回"); assert(!wifi_setup_ui_active()); busy=false;
    busy_failed=true; tap(134,38); assert(wifi_setup_ui_active()); click("圆屏输入");
    assert(find(lv_layer_top(),"语音任务忙，请稍后重试") && !find(lv_layer_top(),"原网络暂未连接"));
    shot("busy-operation.ppm"); click("返回"); phase(WifiSetupPhase::Closed); busy_failed=false;
    tap(134,38); assert(wifi_setup_ui_active()); click("圆屏输入"); shot("preparing.ppm");
    phase(WifiSetupPhase::Scanning); shot("scanning.ppm"); aps(); shot("networks.ppm");
    for(unsigned i=0;i<6;++i) click("下页"); shot("networks-last.ppm");
    select_row(1); shot("ssid-hex-32.ppm"); click("返回");
    for(unsigned i=0;i<6;++i) click("上页");
    select_row(0); assert(find(lv_layer_top(),"中国")); shot("ssid-chinese.ppm"); click("返回");
    select_row(1); shot("ssid-32.ppm"); assert(find(lv_layer_top(),"0123456789abcdefghijklmnopqrstuv")); click("返回");
    select_row(2); shot("ssid-hex.ppm"); assert(find(lv_layer_top(),"F0 9F 98 80 FF"));
    click("连接"); assert(joins==1 && submitted.empty() && submitted_index==2);
    shot("connecting.ppm"); click("取消"); assert(cancels==1); shot("restoring.ppm");
    state.error=WifiSetupError::Cancelled; phase(WifiSetupPhase::Failed); shot("cancelled.ppm");
    click("刷新"); aps(); click("下页"); select_row(0); assert(find(lv_layer_top(),"不支持此认证方式")); click("返回"); click("上页");
    edit_first(); shot("keyboard-lower.ppm");
    // Every available character is selected through LVGL's actual pointer device.
    WifiSetupKeyboard layout;
    bool seen[127]={};
    for(unsigned mode=0;mode<4;++mode) {
        for(unsigned p=0;p<layout.pages();++p) {
            bounds(lv_layer_top());
            for(unsigned i=0;i<15;++i) if(char c=layout.key(i)) {
                tap(76+(i%5)*52,138+(i/5)*48); seen[unsigned(c)]=true;
                // Keep input below 64 while checking each concrete submitted byte.
                for(unsigned n=0;n<7;++n) tap(76,138);
                click("连接"); assert(submitted.size()==8 && submitted[0]==c);
                state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed);
                click("重试"); clear(8);
            }
            click("翻页"); layout.next_page();
        }
        click("模式"); layout.next_mode();
    }
    for(unsigned c=32;c<=126;++c) assert(seen[c]);
    for(unsigned i=0;i<7;++i) click("a"); const auto before=joins; click("连接"); assert(joins==before);
    click("a"); click("连接"); assert(joins==before+1 && submitted=="aaaaaaaa");
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); shot("failure.ppm"); click("重试");
    for(unsigned i=8;i<63;++i) click("a"); click("连接"); assert(submitted.size()==63);
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); click("重试");
    pointer={180,288}; pressed=LV_INDEV_STATE_PRESSED; for(unsigned i=0;i<12;++i) tick();
    pressed=LV_INDEV_STATE_RELEASED; tick(); click("连接"); assert(submitted.size()>=8 && submitted.size()<=61);
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); click("重试");
    clear(submitted.size()); for(unsigned i=0;i<63;++i) click("a");
    click("a"); click("连接"); assert(submitted.size()==64); // 64 hex accepted.
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); click("重试");
    click("删除"); click("g"); const auto invalid=joins; click("连接"); assert(joins==invalid);
    click("b"); click("删除"); click("a"); click("连接"); assert(joins==invalid+1 && submitted.size()==64); // No truncation at 65.
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); click("重试"); shot("keyboard-64.ppm");
    click("返回"); select_row(1); click("密码"); assert(find(lv_layer_top(),"  0/64"));
    click("模式"); shot("keyboard-upper.ppm"); click("模式"); shot("keyboard-number.ppm");
    click("模式"); shot("keyboard-symbol.ppm"); click("翻页"); shot("keyboard-symbol-2.ppm"); click("翻页"); shot("keyboard-symbol-3.ppm");
    click("返回");
    // Press an old row, replace the scan while held, release: no wrong AP selection.
    pointer={180,116}; pressed=LV_INDEV_STATE_PRESSED; tick(); ++state.scan_revision; ++state.revision;
    pressed=LV_INDEV_STATE_RELEASED; tick(); assert(find(lv_layer_top(),"刷新"));
    pointer={180,116}; pressed=LV_INDEV_STATE_PRESSED; tick(); ++state.operation;
    pressed=LV_INDEV_STATE_RELEASED; tick(); --state.operation; assert(find(lv_layer_top(),"刷新"));
    defer_scan=true; click("刷新"); assert(scans>=2); assert(!find(lv_layer_top(),"下页"));
    select_row(0); assert(!find(lv_layer_top(),"密码")); defer_scan=false; aps(0); shot("empty.ppm");
    click("刷新"); state.error=WifiSetupError::ScanFailed; phase(WifiSetupPhase::Failed); shot("scan-failed.ppm");
    click("刷新"); aps(); edit_first(); for(unsigned i=0;i<8;++i) click("a"); click("连接");
    phase(WifiSetupPhase::Saving); shot("saving.ppm"); assert(!find(lv_layer_top(),"取消"));
    phase(WifiSetupPhase::Connected); shot("connected.ppm"); click("完成"); assert(wifi_setup_ui_active());
    phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active()); assert(closes==2);
    assert(underlying==0 && voice_actions==0 && voice_holds==0);
    assert(voice_ui_active() && voice_state.phase==VoicePhase::UploadFailed);
    assert(find(lv_screen_active(),"连接状态未确认"));
    const auto menu_operation=state.operation;
    tap(134,38); assert(state.operation==menu_operation); click("返回"); assert(!wifi_setup_ui_active());
    tap(134,38); click("手机配网"); shot("phone-preparing.ppm");
    phase(WifiSetupPhase::Scanning); shot("phone-initial-scan.ppm"); assert(!find(lv_layer_top(),"下一步"));
    state.phone_phase=WifiPhonePhase::Serving; state.remaining_seconds=299; aps();
    lv_mem_monitor(&memory);
    printf("Combined real voice/Wi-Fi UI after QR live heap: %zu bytes\n", memory.total_size-memory.free_size);
    shot("phone-join.ppm"); qr_shot("qr-wifi.ppm"); assert(find(lv_layer_top(),"1 加入临时 Wi-Fi"));
    state.remaining_seconds=298; tick(); assert(find(lv_layer_top(),"4:58"));
    click("下一步"); shot("phone-web.ppm"); qr_shot("qr-url.ppm"); assert(find(lv_layer_top(),"2 打开配网页面"));
    for(unsigned i=0;i<20;++i) tick(); assert(find(lv_layer_top(),"2 打开配网页面"));
    click("上一步"); assert(find(lv_layer_top(),"1 加入临时 Wi-Fi"));
    phase(WifiSetupPhase::Connecting); shot("phone-connecting.ppm"); click("取消");
    assert(find(lv_layer_top(),"正在恢复原网络"));
    state.error=WifiSetupError::Authentication; phase(WifiSetupPhase::Failed); shot("phone-failed.ppm");
    assert(!find(lv_layer_top(),"重试") && !find(lv_layer_top(),"密码"));
    click("二维码"); shot("phone-retry.ppm"); assert(find(lv_layer_top(),"1 加入临时 Wi-Fi"));
    click("下一步"); phase(WifiSetupPhase::Connecting); phase(WifiSetupPhase::Saving);
    shot("phone-saving.ppm"); assert(!find(lv_layer_top(),"取消"));
    state.remaining_seconds=30; phase(WifiSetupPhase::Connected); shot("phone-connected.ppm");
    state.phone_phase=WifiPhonePhase::Closing; ++state.revision; tick(); shot("phone-cleanup.ppm");
    state.phone_phase=WifiPhonePhase::Off; state.original_connected=true; ++state.revision; tick();
    assert(wifi_setup_ui_active() && find(lv_layer_top(),"已连接 Wi-Fi"));
    state.original_connected=false; ++state.revision; tick();
    assert(find(lv_layer_top(),"配置已保存\n当前未连接")); shot("phone-saved-offline.ppm");
    click("完成"); assert(!wifi_setup_ui_active());
    tap(134,38); click("手机配网"); click("取消"); phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active());
    tap(134,38); click("手机配网"); state.phone_phase=WifiPhonePhase::Serving; aps(); click("退出");
    shot("phone-exit.ppm"); phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active());
    tap(134,38); click("手机配网"); state.phone_phase=WifiPhonePhase::Serving; aps();
    state.remaining_seconds=0; phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active());
    tap(134,38); click("手机配网"); state.phone_phase=WifiPhonePhase::Serving; phase(WifiSetupPhase::Connected);
    click("完成"); state.phone_phase=WifiPhonePhase::Closing; ++state.revision; tick();
    assert(wifi_setup_ui_active()); state.phone_phase=WifiPhonePhase::Off; phase(WifiSetupPhase::Connected);
    assert(!wifi_setup_ui_active());
    busy_failed=true; tap(134,38); click("手机配网"); assert(find(lv_layer_top(),"语音任务忙，请稍后重试"));
    click("返回"); phase(WifiSetupPhase::Closed); busy_failed=false;
    assert(underlying==0 && voice_actions==0 && voice_holds==0);
    assert(voice_ui_active() && voice_state.phase==VoicePhase::UploadFailed);
    voice_state.phase=VoicePhase::Idle; ++voice_state.generation; tick();
    tap(180,180); assert(underlying==1);
    tap(134,38); click("圆屏输入"); click("取消"); assert(wifi_setup_ui_active()); phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active());
    for(unsigned i=0;i<10;++i) {
        tap(134,38); click("圆屏输入"); phase(WifiSetupPhase::Scanning); aps(20); click("返回");
        assert(wifi_setup_ui_active()); phase(WifiSetupPhase::Closed); assert(!wifi_setup_ui_active());
    }
    assert(underlying==1 && voice_actions==0 && voice_holds==0);
    tap(134,38); click("手机配网"); state.phone_phase=WifiPhonePhase::Serving; aps();
    for(int i=0;i<100;++i) tick();
    assert(wifi_setup_ui_active() && find(lv_layer_top(),"1 加入临时 Wi-Fi"));
    click("退出"); phase(WifiSetupPhase::Closed);
    assert(!wifi_setup_ui_active() && !find(lv_screen_active(),"按住说话"));
    tap(180,180); tick();
    for(bool operation:{false,true}) {
        voice_state.phase=VoicePhase::Claimed; ++voice_state.generation; tick();
        tap(134,38); tick();
        if(operation) click("圆屏输入");
        assert(lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        const auto covered=character.pixels;
        const auto actions_before=voice_actions;
        voice_state.phase=VoicePhase::Replied; ++voice_state.generation;
        for(int i=0;i<8;++i) tick();
        assert(character.pixels==covered && voice_actions==actions_before);
        if(operation) { click("取消"); phase(WifiSetupPhase::Closed); }
        else click("返回");
        tick(); lv_refr_now(nullptr);
        assert(!wifi_setup_ui_active() && !lv_obj_has_flag(character.canvas,LV_OBJ_FLAG_HIDDEN));
        assert(character.pixels!=covered && voice_actions==actions_before);
        assert(find(lv_screen_active(),"收到回复"));
        shot(operation ? "fusion-operation-close.ppm" : "fusion-menu-return.ppm");
    }
    puts("Fused Wi-Fi cover PASS: menu return and operation close restore current pose without hidden rendering or delayed auto-open");
    printf("Wi-Fi UI real LVGL PASS: ASCII 32–126, pointer input, 7/8/63/64/65 boundaries, masked input, scan generations, failures/retry/cancel/close, circle target geometry, no event passthrough, method menu, static phone QR pages, countdown, phone failure/reconnect/close/success\n");
}
