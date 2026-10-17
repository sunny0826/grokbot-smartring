#include "voice_character.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<uint16_t> draw(VoiceCharacterMotion &motion, uint32_t now)
{
    std::vector<uint16_t> pixels(320 * 320);
    Fb fb{pixels.data(), 320, 320};
    auto params = motion.sample(fb, now);
    fb_clear(fb, params.paper);
    character_draw(params);
    return pixels;
}

int main()
{
    const std::array phases = {VoicePhase::Preparing, VoicePhase::Recording, VoicePhase::Ready,
        VoicePhase::Uploading, VoicePhase::Stored, VoicePhase::Transcribing, VoicePhase::Waiting,
        VoicePhase::Claimed, VoicePhase::Replied, VoicePhase::LoadingPage,
        VoicePhase::UploadFailed, VoicePhase::ReplyFailed, VoicePhase::Failed,
        VoicePhase::NeedsReview, VoicePhase::Unavailable};
    unsigned frames = 0;
    for (int skin = 0; skin < character_count(); ++skin) {
        character_select(skin);
        VoiceCharacterMotion motion;
        CharacterRenderer incremental;
        std::vector<uint16_t> actual(320 * 320, 0xdead), expected(actual.size());
        uint32_t now = 0xfffff000;
        for (auto phase : phases) {
            for (int frame = 0; frame < 40; ++frame) {
                now += 33;
                VoicePresentation input{CharacterMode::Voice, phase, HoldVisual::None,
                    phase == VoicePhase::Recording ? (frame % 20) / 20.f : 0};
                motion.observe(input, now);
                Fb actual_fb{actual.data(), 320, 320};
                auto params = motion.sample(actual_fb, now);
                incremental.render(params);
                params.fb = {expected.data(), 320, 320};
                fb_clear(params.fb, params.paper);
                PixelRect written;
                params.fb.writes = &written;
                character_draw(params);
                assert(actual == expected);
                if (phase == VoicePhase::LoadingPage) {
                    assert(written.x1 >= 55 && written.x2 <= 265);
                    assert(written.y1 >= 75 && written.y2 <= 270);
                } else {
                    assert(written.x1 >= 85 && written.x2 <= 235);
                    assert(written.y1 >= 65 && written.y2 <= 200);
                }
                ++frames;
            }
        }
        for (auto phase : {VoicePhase::Ready, VoicePhase::UploadFailed, VoicePhase::ReplyFailed,
                           VoicePhase::Failed, VoicePhase::NeedsReview, VoicePhase::Unavailable}) {
            now += 100;
            motion.observe({CharacterMode::Voice, phase}, now);
            assert(!motion.moving(now));
            const auto still = draw(motion, now);
            assert(draw(motion, now + 30000) == still);
        }
        now += 100;
        motion.observe({CharacterMode::Voice, VoicePhase::Idle, HoldVisual::Cancelled}, now);
        assert(!motion.moving(now));
        assert(draw(motion, now) == draw(motion, now + 30000));
        now += 100;
        motion.observe({CharacterMode::Voice, VoicePhase::Recording}, now);
        const auto silence = draw(motion, now + 300);
        assert(draw(motion, now + 900) == silence);
        motion.observe({CharacterMode::Voice, VoicePhase::Recording, HoldVisual::None, 1}, now + 900);
        assert(draw(motion, now + 900) != silence);
        motion.observe({CharacterMode::Voice, VoicePhase::Waiting}, now + 1000);
        const auto waiting = draw(motion, now + 2000);
        motion.observe({CharacterMode::Voice, VoicePhase::Claimed}, now + 3000);
        assert(draw(motion, now + 4000) != waiting);
        const auto thinking = motion.sample({nullptr, 320, 320}, now + 4000);
        assert(thinking.pose->bodyR < .25f && thinking.pose->eyeAlpha == 0);
        assert(thinking.pose->dotR[0] > .15f && thinking.pose->dotR[1] > .15f);
        const auto thinking_frame = draw(motion, now + 4000);
        assert(draw(motion, now + 4250) != thinking_frame);
        for (auto phase : {VoicePhase::Uploading, VoicePhase::Stored, VoicePhase::Transcribing,
                           VoicePhase::Waiting, VoicePhase::LoadingPage}) {
            VoiceCharacterMotion ongoing;
            ongoing.observe({CharacterMode::Voice, phase}, 0);
            const auto at_thirty_seconds = ongoing.sample({nullptr, 320, 320}, 30000);
            assert(at_thirty_seconds.pose->ringA == 1.f);
            const auto previous_frame = draw(ongoing, 30000);
            assert(draw(ongoing, 30330) != previous_frame);
        }
        motion.observe({CharacterMode::Covered, VoicePhase::Replied}, now + 5000);
        motion.observe({CharacterMode::Voice, VoicePhase::Replied}, now + 10000);
        assert(!motion.moving(now + 10000));
        assert(draw(motion, now + 10000) == draw(motion, now + 11000));
    }
    BotPose one_shot;
    poseFor(BotState::Orbit, 5, one_shot);
    assert(one_shot.ringA == 0);
    printf("PASS: %u fused character pixel comparisons, three skins, original thinking/notify, sustained orbit, static errors, real level and tick wrap\n", frames);
}
