#include "fixture.hpp"
namespace fixture {
void run_actor_scenario(const std::string &name) {
    if (name == "scope-ready") {
        voice_start(); record(true); host::fast_capture = true;
        require(eventually([] { return voice_snapshot().phase == VoicePhase::Ready; }, 5000), "capture did not reach limit");
        require(voice_settings_pause(1), "cannot pause Ready");
        require(eventually([] { return voice_settings_status(1) == VoiceSettingsStatus::Paused; }), "pause deadline");
        const auto generation = voice_snapshot().generation;
        scope_version = 2; voice_settings_resume(1);
        require(eventually([&] { return voice_snapshot().generation != generation; }), "scope not refreshed");
        action(VoiceAction::Boot); phase(VoicePhase::UploadFailed);
        require(uploaded_id().empty(), "old recording delivered to new enrollment");
        require(persisted().pending().scope == scope, "pending lost its original enrollment");
        require(voice_settings_pause(2), "cannot pause failed upload");
        require(eventually([] { return voice_settings_status(2) == VoiceSettingsStatus::Paused; }), "pause deadline");
        const auto retry_generation = voice_snapshot().generation;
        scope_version = 1; voice_settings_resume(2);
        require(eventually([&] { return voice_snapshot().generation != retry_generation; }), "restored scope not refreshed");
        action(VoiceAction::Retry); phase(VoicePhase::Stored);
        require(!uploaded_id().empty(), "same enrollment retry failed"); return;
    }
    if (name == "corrupt-start") {
        host::seed_nvs("voice", "journal_v1", "broken");
        voice_start(); phase(VoicePhase::Unavailable);
        require(std::strcmp(voice_snapshot().error, "storage_unavailable") == 0, "missing storage failure");
        require(reads == 0 && inboxes == 0, "corrupt journal performed mailbox work");
        return;
    }
    if (name == "restart-read" || name == "restart-confirm" || name == "restart-empty") {
        auto journal = persisted();
        require(!journal.pending().id[0], "read item recovered as active pending");
        require(journal.size() == (name == "restart-empty" ? 0u : 1u), "wrong receipt count after process restart");
        if (name == "restart-confirm") block_read = false;
        voice_start();
        if (name == "restart-empty") {
            require(eventually([] { return inboxes > 0; }), "empty restart did not poll");
            require(reads == 0, "confirmed receipt retried after restart");
        } else {
            require(eventually([] { return reads > 0; }), "persisted read was not retried");
            if (name == "restart-confirm") require(eventually([] { return persisted().size() == 0; }), "retry not durably removed");
        }
        require(voice_snapshot().phase == VoicePhase::Idle && !voice_snapshot().unread, "read resurrected after restart");
        return;
    }
    if (name == "inbox-hold" || name == "inbox-boot" || name == "stale-inbox") {
        block_inbox = true;
        if (name == "stale-inbox") set_inbox({A});
        voice_start();
        require(eventually([] { return inboxes == 1; }), "Inbox barrier not reached");
        const auto hold = record(name == "inbox-boot");
        cancel(hold);
        if (name == "stale-inbox") {
            set_inbox({}); release_inbox();
            bool resurrected = false;
            require(eventually([&] {
                resurrected |= voice_snapshot().unread;
                return inboxes >= 2;
            }), "stale Inbox was not replaced");
            require(!resurrected && !voice_snapshot().unread, "old Inbox result resurrected unread");
        }
        return;
    }
    seed_pending(); voice_start(); open_reply();
    if (name == "save-failure") {
        host::fail_commit = true; action(VoiceAction::MarkRead);
        require(eventually([] { return voice_snapshot().error[0]; }), "save error not published");
        auto snapshot = voice_snapshot();
        require(snapshot.phase == VoicePhase::ViewingReply, "save failure lost page");
        require(persisted().size() == 0 && std::strcmp(persisted().pending().id, A) == 0, "failed enqueue changed durable state");
        std::vector<uint8_t> page(VOICE_PAGE_BYTES);
        require(voice_copy_page(snapshot.generation, page.data(), page.size()), "save failure lost page bytes");
        require(reads == 0, "failed local save sent network acknowledgement");
        host::fail_commit = false; mark(); return;
    }
    mark();
    if (name == "restart-write") {
        require(persisted().size() == 1 && !persisted().pending().id[0], "local read not persisted atomically"); return;
    }
    if (name == "read-hold" || name == "read-boot") {
        const auto hold = record(name == "read-boot");
        require(read_permits == 0, "network barrier released before recording");
        cancel(hold); release_read();
        require(eventually([] { return persisted().size() == 0; }), "read not confirmed"); return;
    }
    if (name == "late-read") {
        const auto hold = record();
        require(eventually([] { return voice_snapshot().duration_ms >= 400; }, 5000), "recording did not capture audio");
        voice_hold_end(hold, VoiceHoldEnd::Send); phase(VoicePhase::Stored);
        const auto id = uploaded_id(); require(!id.empty() && id != A, "new upload did not use new ID");
        require(persisted().pending().id == id, "new recording not persisted");
        release_read();
        require(eventually([] { return persisted().size() == 0; }), "old read completion not consumed");
        require(voice_snapshot().phase == VoicePhase::Stored && persisted().pending().id == id,
                "Read A completion changed new B pending or phase"); return;
    }
    if (name == "delete-failure") {
        host::fail_commit = true; release_read();
        require(eventually([] { return host::failed_commits > 0; }), "delete failure not exercised");
        require(persisted().contains(receipt(A)), "failed delete lost receipt");
        host::fail_commit = false; host::advance_ms(5000);
        require(eventually([] { return reads >= 2; }), "failed deletion not retried");
        release_read(); require(eventually([] { return persisted().size() == 0; }), "delete retry not persisted"); return;
    }
    if (name == "multi-unread") {
        read_success = false; set_inbox({A, B}); release_read();
        require(eventually([] { return voice_snapshot().unread; }), "B not discoverable while A retry pending");
        require(last_exclusions == 1, "Inbox did not carry A exclusion");
        action(VoiceAction::Open); phase(VoicePhase::ViewingReply);
        action(VoiceAction::MarkRead); phase(VoicePhase::Idle);
        require(persisted().contains(receipt(A)) && persisted().contains(receipt(B)), "multi-read queue lost A or B"); return;
    }
    throw std::runtime_error("unknown actor scenario: " + name);
}
}
