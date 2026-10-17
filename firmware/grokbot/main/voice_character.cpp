#include "voice_character.h"
#include <cmath>

bool VoiceCharacterMotion::observe(VoicePresentation input, uint32_t now)
{
    const bool changed = input.phase != input_.phase || input.hold != input_.hold;
    if (changed) {
        from_ = pose_;
        entered_ = now;
        morph_ = sampled_ && input_.mode == CharacterMode::Voice && input.mode == CharacterMode::Voice;
    }
    if (input.mode != CharacterMode::Voice) {
        sampled_ = morph_ = false;
    }
    input_ = input;
    return changed;
}

bool VoiceCharacterMotion::moving(uint32_t now) const
{
    if (input_.hold == HoldVisual::Cancelled) return false;
    if (input_.hold == HoldVisual::Preparing) return true;
    switch (input_.phase) {
    case VoicePhase::Preparing:
    case VoicePhase::Recording:
    case VoicePhase::Uploading:
    case VoicePhase::Stored:
    case VoicePhase::Transcribing:
    case VoicePhase::Waiting:
    case VoicePhase::Claimed:
    case VoicePhase::LoadingPage:
        return true;
    case VoicePhase::Replied:
        return now - entered_ < 500;
    default:
        return false;
    }
}

CharacterDrawParams VoiceCharacterMotion::sample(Fb fb, uint32_t now)
{
    const float t = (now - entered_) / 1000.f;
    const bool animated = moving(now);
    BotPose target;
    poseFor(BotState::Idle, 0, target);
    target.gaze = {0, 8, 0};
    float sx = 1, sy = 1, lid = 1;
    if (animated && input_.phase != VoicePhase::Recording) {
        const auto life = liveliness(t, .25f, true, false);
        lid = life.lid;
        target.bodyR = 1.f + .015f * sinf(t * 2.f);
    }
    switch (input_.phase) {
    case VoicePhase::Recording:
        sx = 1.f + .10f * input_.level;
        sy = 1.f - .07f * input_.level;
        target.bodyCy = -.06f * input_.level;
        target.gaze.pitch = 8.f + 14.f * input_.level;
        break;
    case VoicePhase::Uploading:
    case VoicePhase::Transcribing:
    case VoicePhase::Stored:
    case VoicePhase::Waiting:
    case VoicePhase::LoadingPage:
        poseFor(BotState::Orbit, t, target, PosePlayback::Continuous);
        break;
    case VoicePhase::Claimed:
        poseFor(BotState::Thinking, t, target);
        break;
    case VoicePhase::Replied:
        poseFor(BotState::Notify, fminf(t, .5f), target);
        break;
    default:
        break;
    }
    if (morph_ && animated && t < .18f) blendPose(pose_, from_, target, easeOutCubic(t / .18f));
    else pose_ = target;
    sampled_ = true;
    const bool reading_reply = input_.phase == VoicePhase::LoadingPage;
    const float scale = reading_reply ? 64.f : 44.f;
    const float center_y = reading_reply ? 173.f : 133.f;
    return {fb, fb.w * .5f + pose_.bodyCx * scale, center_y + pose_.bodyCy * scale,
        0, scale * pose_.bodyR * sx, scale * pose_.bodyR * sy, sx, 0, sy,
        pose_.gaze, &pose_, lid, scale, t,
        rgb565(0x0a, 0x0a, 0x0c), rgb565(0xec, 0xe7, 0xdb), 1, 0};
}
