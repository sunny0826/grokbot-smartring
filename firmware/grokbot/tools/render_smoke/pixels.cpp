#include "render_cases.h"
#include "legacy_frame.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

#ifdef NDEBUG
#error "Pixel checks require assertions"
#endif
namespace {
constexpr int SIDE = 320, COUNT = SIDE * SIDE, GUARD = 32;
constexpr uint16_t SENTINEL = 0x55aa, PAPER = 0xef3b;
std::vector<uint16_t> actual(COUNT + 2*GUARD, SENTINEL), reference(COUNT), panel(COUNT, SENTINEL);
Fb incremental{actual.data()+GUARD, SIDE, SIDE}, full{reference.data(), SIDE, SIDE};
CharacterRenderer renderer;
size_t frames;
uint64_t dirty_pixels;

void check(CharacterDrawParams params, bool reset = false)
{
    if (reset) renderer.reset();
    params.fb = full;
    fb_clear(full, params.paper);
    character_draw(params);
    params.fb = incremental;
    PixelRect dirty = renderer.render(params);
    assert(params.fb.writes == nullptr);
    if (reset) assert(dirty.area() == COUNT);
    assert(dirty.empty() || (dirty.x1 >= 0 && dirty.y1 >= 0 && dirty.x2 < SIDE && dirty.y2 < SIDE));
    for (int y=dirty.y1; !dirty.empty() && y<=dirty.y2; ++y)
        std::copy(incremental.px+y*SIDE+dirty.x1, incremental.px+y*SIDE+dirty.x2+1,
                  panel.data()+y*SIDE+dirty.x1);
    for (int i=0; i<COUNT; ++i) {
        if (incremental.px[i] != reference[i] || panel[i] != reference[i]) {
            std::fprintf(stderr, "mismatch frame=%zu skin=%d at=%d,%d dirty=%d,%d,%d,%d\n",
                frames, character_index(), i%SIDE, i/SIDE, dirty.x1,dirty.y1,dirty.x2,dirty.y2);
            std::abort();
        }
    }
    assert(std::all_of(actual.begin(),actual.begin()+GUARD,[](auto p){return p==SENTINEL;}));
    assert(std::all_of(actual.end()-GUARD,actual.end(),[](auto p){return p==SENTINEL;}));
    dirty_pixels += dirty.area();
    ++frames;
}

void primitive_checks()
{
    std::vector<uint16_t> pixels(17*19, SENTINEL);
    PixelRect writes;
    Fb fb{pixels.data(), 17, 19, &writes};
    fb_clear(fb, 0);
    assert(writes.area() == 17*19 && pixels.back() == 0);
    writes = {};
    fb_blend_px(fb,-1,0,0xffff,1); fb_blend_px(fb,17,0,0xffff,1);
    fb_fill_ellipse(fb,5,5,0,3,3,0xffff,0);
    fb_clear_rect(fb,8,8,2,2,1);
    assert(writes.empty());
    fb_blend_px(fb,16,18,0xffff,1);
    assert(writes.area()==1 && writes.x1==16 && writes.y1==18 && pixels.back()==0xffff);
    writes = {};
    fb_fill_ellipse(fb,-2,2,.7f,5,3,0xffff,1);
    fb_draw_ellipse_over(fb,15,16,.7f,5,3,0xffff,0);
    fb_fill_capsule(fb,0,17,.7f,.7f,-.7f,.7f,5,2,0x1234,.8f);
    float xs[] = {-5,8,25}, ys[] = {1,22,3};
    fb_fill_poly(fb,xs,ys,3,0xf000,.8f);
    for(int y=0;y<19;++y) for(int x=0;x<17;++x)
        if(pixels[y*17+x]) assert(x>=writes.x1 && x<=writes.x2 && y>=writes.y1 && y<=writes.y2);
}
}

int main()
{
    primitive_checks();
    std::vector<uint16_t> baseline(COUNT);
    LegacyFrame legacy;
    for(int skin=0;skin<character_count();++skin) {
        character_select(skin);legacy.reset();
        for(int i=0;i<150;++i) {
            BotPose pose;poseFor(BotState::Idle,0,pose);
            auto params=render_case(full,pose,i/30.f);
            fb_clear(full,params.paper);character_draw(params);
            params.fb={baseline.data(),SIDE,SIDE};legacy.render(params);
            assert(baseline==reference);
        }
    }
    for(int skin=0;skin<character_count();++skin) {
        character_select(skin);
        for(int from=0;from<static_cast<int>(BotState::COUNT);++from)
            for(int to=0;to<static_cast<int>(BotState::COUNT);++to) {
                for(int frame=0;frame<15;++frame) {
                    float t=frame*.17f;
                    BotPose a,b,pose;
                    poseFor(static_cast<BotState>(from), t, a);
                    poseFor(static_cast<BotState>(to), t, b);
                    blendPose(pose,a,b,frame/14.f);
                    check(render_case(incremental,pose,t,frame%3==0 ? 60.f : -20.f),frame==0);
                }
            }
    }
    std::mt19937 random(0x5a0c);
    std::uniform_real_distribution<float> unit(0,1);
    for(int i=0;i<400;++i) {
        character_select(random()%character_count());
        BotPose pose; float t=unit(random)*20;
        poseFor(static_cast<BotState>(random()%static_cast<int>(BotState::COUNT)),t,pose);
        auto params=render_case(incremental,pose,t,unit(random)*120-60);
        if(i%13==0) params.rcx=-30;
        if(i%17==0) params.rcy=350;
        check(params,i%31==0);
    }
    for(float alpha : {1.f,.5f,.011f,.01f,.001f,0.f}) {
        BotPose pose;poseFor(BotState::Sleepy,2.19f,pose);pose.zzzA=alpha;
        check(render_case(incremental,pose,2.19f));
        check(render_case(incremental,pose,2.21f));
    }
    BotPose empty{};
    auto params=render_case(incremental,empty,0);params.erx=params.ery=0;
    check(params);check(params);
    assert(renderer.render(params).empty());
    check(params,true);
    std::printf("PASS: %zu full-frame and simulated-panel comparisons, mean dirty %.1f%%, all skins/states/transitions, guards and reset\n",
                frames,100.0*dirty_pixels/(frames*COUNT));
}
