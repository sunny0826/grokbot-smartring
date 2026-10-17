#pragma once

#include "voice.h"
#include "bot_character.h"

enum class CharacterMode { Home, Voice, Covered };
enum class HoldVisual { None, Preparing, Cancelled };

struct VoicePresentation {
    CharacterMode mode = CharacterMode::Home;
    VoicePhase phase = VoicePhase::Idle;
    HoldVisual hold = HoldVisual::None;
    float level = 0;
    bool unread = false;
};

class VoiceCharacterMotion {
public:
    bool observe(VoicePresentation input, uint32_t now);
    bool moving(uint32_t now) const;
    CharacterDrawParams sample(Fb fb, uint32_t now);
private:
    VoicePresentation input_;
    uint32_t entered_ = 0;
    BotPose pose_{}, from_{};
    bool sampled_ = false, morph_ = false;
};
