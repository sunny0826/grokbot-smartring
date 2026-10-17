#include "bot_motion.h"
#include <cassert>
#include <cmath>

int main()
{
    BodyMotion one, four;
    one.advance(.1f,.2f,.3f,false);
    for(int i=0;i<4;++i)four.advance(.025f,.2f,.3f,false);
    assert(fabsf(one.x-four.x)<1e-6f && fabsf(one.y-four.y)<1e-6f);
    BodyMotion slow, fast;
    for(int i=0;i<15;++i)slow.advance(1.f/15,.1f,.2f,false);
    for(int i=0;i<60;++i)fast.advance(1.f/60,.1f,.2f,false);
    assert(fabsf(slow.y-fast.y)<.005f);
    BodyMotion stalled, capped;
    stalled.advance(5,9.81f,0,false);capped.advance(.25f,9.81f,0,false);
    assert(stalled.x==capped.x && stalled.vx==capped.vx);
    for(int i=0;i<1000;++i) {
        stalled.advance(.033f,9.81f*cosf(i),9.81f*sinf(i),false);
        assert(std::isfinite(stalled.x) && std::isfinite(stalled.vx));
        assert(hypotf(stalled.x,stalled.y)<=.52001f);
    }
    const float before=hypotf(stalled.x,stalled.y);
    stalled.advance(.1f,9,9,true);
    assert(hypotf(stalled.x,stalled.y)<before && stalled.vx==0 && stalled.vy==0);
}
