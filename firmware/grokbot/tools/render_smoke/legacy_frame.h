#pragma once
#include "bot_character.h"
#include <cmath>

class LegacyFrame {
    PixelRect previous_;
    bool first_ = true;
public:
    void reset() { first_ = true; }
    void render(CharacterDrawParams params) {
        auto &fb=params.fb;
        fb.writes=nullptr;
        const float c=cosf(params.erot),s=sinf(params.erot);
        const float x=sqrtf(params.erx*params.erx*c*c+params.ery*params.ery*s*s);
        const float y=sqrtf(params.erx*params.erx*s*s+params.ery*params.ery*c*c);
        PixelRect current{std::max(0,int(floorf(params.rcx-x))-3),std::max(0,int(floorf(params.rcy-y))-3),
            std::min(fb.w-1,int(ceilf(params.rcx+x))+3),std::min(fb.h-1,int(ceilf(params.rcy+y))+3)};
        if(character_index()!=0) {
            fb_clear(fb,params.paper);first_=true;
        } else if(first_) {
            fb_clear(fb,params.paper);first_=false;
        } else if(current.x1!=previous_.x1 || current.y1!=previous_.y1 ||
                  current.x2!=previous_.x2 || current.y2!=previous_.y2) {
            fb_clear_rect(fb,previous_.x1,previous_.y1,previous_.x2,previous_.y2,params.paper);
        }
        character_draw(params);previous_=current;
    }
};
