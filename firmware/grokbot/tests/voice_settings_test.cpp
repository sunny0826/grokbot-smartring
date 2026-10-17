#include "voice_settings.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main()
{
    VoiceSettingsGate gate;
    assert(!gate.request(1, VoicePhase::Recording, false));
    assert(!gate.request(1, VoicePhase::Preparing, false));
    assert(!gate.request(1, VoicePhase::Idle, true));
    assert(!gate.blocks_input());
    // Dequeue and reservation share a lock in the worker; Close cannot race an accepted pause.
    assert(gate.begin_work(VoiceSettingsWork::Input));
    assert(!gate.request(1, VoicePhase::UploadFailed, false));
    assert(gate.request(1, VoicePhase::Uploading, false));
    assert(!gate.boundary(VoicePhase::Uploading, false));
    gate.finish_work();
    assert(gate.boundary(VoicePhase::UploadFailed, false));
    gate.resume(1);
    assert(gate.begin_work(VoiceSettingsWork::Poll));
    assert(gate.request(1, VoicePhase::Idle, false));
    assert(!gate.boundary(VoicePhase::Idle, false));
    gate.finish_work();
    assert(!gate.begin_work(VoiceSettingsWork::Poll));
    assert(gate.boundary(VoicePhase::Idle, false));
    gate.resume(1);
    // An in-flight HTTP operation must reach the worker boundary before radio changes.
    assert(gate.request(1, VoicePhase::Uploading, false));
    assert(gate.status(1) == VoiceSettingsStatus::Pending && gate.blocks_input());
    assert(!gate.request(2, VoicePhase::Idle, false));
    gate.resume(2);
    assert(gate.status(1) == VoiceSettingsStatus::Pending);
    assert(gate.boundary(VoicePhase::UploadFailed, false));
    assert(gate.status(1) == VoiceSettingsStatus::Paused);
    assert(gate.boundary(VoicePhase::UploadFailed, false));
    gate.resume(1);
    assert(!gate.blocks_input() && !gate.boundary(VoicePhase::UploadFailed, false));
    // Cancel while HTTP is still running cannot leave a late pause behind.
    assert(gate.request(2, VoicePhase::LoadingPage, false));
    gate.resume(2);
    assert(!gate.boundary(VoicePhase::ViewingReply, false));
    assert(gate.status(2) == VoiceSettingsStatus::Inactive);
    // An action dequeued just before the request may have started recording.
    assert(gate.request(3, VoicePhase::Idle, false));
    assert(!gate.boundary(VoicePhase::Recording, true));
    assert(gate.status(3) == VoiceSettingsStatus::Busy);
    gate.resume(3);
    assert(!gate.blocks_input());
    for (auto phase : {VoicePhase::Ready, VoicePhase::UploadFailed, VoicePhase::Waiting,
                       VoicePhase::Claimed, VoicePhase::ViewingReply}) {
        assert(gate.request(4, phase, false));
        assert(gate.boundary(phase, false));
        gate.resume(4);
    }
    puts("Voice settings pause: in-flight HTTP boundary, retained states, input exclusion and stale/cancelled lease PASS");
}
