#include "device_benchmark.h"
#include "render_cases.h"
#include "legacy_frame.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace {
constexpr int BLOCKS=10;
constexpr int64_t WARMUP=1000000, MEASURE=15000000;
struct Stats {
    uint32_t frames=0, failed=0, strips=0, te_timeouts=0, renders=0;
    uint64_t bytes=0, render_us=0, te_us=0, refresh_us=0;
    int64_t first_done=0, last_done=0;
    uint32_t interval_ms[256]{};
    uint32_t min_internal=UINT32_MAX, min_psram=UINT32_MAX;
    bool interrupted=false;
} stats;
LegacyFrame legacy;
int block=-1, original_skin;
int64_t boot=0, block_started=0, frame_started=0;
bool measuring=false, frame_failed=false, finished=false;
uint64_t frame_bytes=0;
uint32_t frame_strips=0;

bool full_mode() { return (block/2)%2==block%2; }
uint32_t percentile(float fraction) {
    uint32_t target=uint32_t(ceilf((stats.frames>0?stats.frames-1:0)*fraction)),sum=0;
    for(uint32_t i=0;i<256;++i) { sum+=stats.interval_ms[i];if(sum>=target)return i; }
    return 255;
}
void report() {
    char p50[12], p95[12];
    const auto quantile = [](float fraction, char *out, size_t size) {
        const uint32_t value = percentile(fraction);
        if (stats.frames < 2 || value == 255) snprintf(out, size, "null");
        else snprintf(out, size, "%" PRIu32, value);
    };
    quantile(.5f, p50, sizeof(p50)); quantile(.95f, p95, sizeof(p95));
    ESP_LOGI("display_benchmark",
        "{\"block\":%d,\"mode\":\"%s\",\"trajectory\":\"three-skins-idle-v1\","
        "\"frames\":%u,\"failed_frames\":%u,\"strips\":%u,\"bytes\":%" PRIu64 ","
        "\"elapsed_us\":%" PRId64 ",\"render_us\":%" PRIu64 ",\"renders\":%u,"
        "\"te_wait_us\":%" PRIu64 ",\"refresh_us\":%" PRIu64 ",\"te_timeouts\":%u,"
        "\"p50_ms_upper\":%s,\"p95_ms_upper\":%s,\"min_internal\":%u,\"min_psram\":%u,\"interrupted\":%s}",
        block,full_mode()?"legacy-full":"tracked-partial",stats.frames,stats.failed,stats.strips,stats.bytes,
        stats.last_done-stats.first_done,stats.render_us,stats.renders,stats.te_us,stats.refresh_us,
        stats.te_timeouts,p50,p95,stats.min_internal,stats.min_psram,
        stats.interrupted?"true":"false");
}
}

void display_benchmark_pause() { if(block>=0 && !finished)stats.interrupted=true; }

bool display_benchmark_render(CharacterDrawParams params, CharacterRenderer &renderer, PixelRect &dirty)
{
    const int64_t now=esp_timer_get_time();
    if(!boot) { boot=now;original_skin=character_index(); }
    if(finished || now-boot<8000000) return false;
    if(block<0 || now-block_started>=WARMUP+MEASURE) {
        if(block>=0)report();
        ++block;renderer.reset();legacy.reset();measuring=false;
        if(block==BLOCKS) {
            finished=true;character_select(original_skin);
            ESP_LOGI("display_benchmark","complete");return false;
        }
        stats={};block_started=now;
    }
    const int64_t elapsed=now-block_started;
    measuring=elapsed>=WARMUP;
    float t=std::max(0.f,float(elapsed-WARMUP)/1000000.f);
    character_select(std::min(2,int(t/5.f)));
    BotPose pose;poseFor(BotState::Idle,0,pose);
    params=render_case(params.fb,pose,fmodf(t,5.f));
    const int64_t begin=esp_timer_get_time();
    if(full_mode()) {
        legacy.render(params);dirty={0,0,params.fb.w-1,params.fb.h-1};
    } else dirty=renderer.render(params);
    const int64_t end=esp_timer_get_time();
    if(measuring) {
        stats.render_us+=end-begin;++stats.renders;
        stats.min_internal=std::min(stats.min_internal,uint32_t(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
        stats.min_psram=std::min(stats.min_psram,uint32_t(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    return true;
}

extern "C" void my_bot_benchmark_flush(size_t bytes, int64_t began, int64_t ended,
    int64_t te_wait_us, bool te_timeout, bool first, bool last, bool success)
{
    if(!measuring || finished)return;
    if(first) { frame_started=began;frame_failed=false;frame_bytes=0;frame_strips=0; }
    frame_failed|=!success;
    if(success) { frame_bytes+=bytes;++frame_strips; }
    stats.te_us+=te_wait_us;stats.te_timeouts+=te_timeout;
    if(!last)return;
    if(frame_failed) { ++stats.failed;return; }
    ++stats.frames;stats.bytes+=frame_bytes;stats.strips+=frame_strips;
    stats.refresh_us+=ended-frame_started;
    if(stats.last_done) {
        const auto interval=uint32_t((ended-stats.last_done+999)/1000);
        ++stats.interval_ms[std::min(uint32_t(255),interval)];
    } else stats.first_done=ended;
    stats.last_done=ended;
}
