#include "fixture.hpp"
#include "voice_scope.h"
namespace fixture {
std::atomic<unsigned> scope_version{1};
std::atomic<unsigned> responses{0}, response_permits{0}, page_downloads{0}, invalidations{0};
std::atomic<bool> block_response{false}, response_ready{false};
void release_response() { ++response_permits; host::network_cv.notify_all(); }
std::atomic<unsigned> reads{0}, inboxes{0}, read_permits{0}, inbox_permits{0}, last_exclusions{0};
std::atomic<bool> block_read{true}, block_inbox{false}, read_success{true}, ignore_exclusions{false};
namespace {
std::vector<std::string> inbox_ids;
std::string upload_id;
std::vector<std::string> server_read;
}
void set_inbox(std::vector<std::string> ids) {
    std::lock_guard lock(host::network_mutex); inbox_ids = std::move(ids);
}
std::string uploaded_id() { std::lock_guard lock(host::network_mutex); return upload_id; }
void release_read() { ++read_permits; host::network_cv.notify_all(); }
void release_inbox() { ++inbox_permits; host::network_cv.notify_all(); }
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
bool eventually(const std::function<bool()> &predicate, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
void phase(VoicePhase expected) {
    if (!eventually([&] { return voice_snapshot().phase == expected; }))
        throw std::runtime_error("phase deadline: expected=" + std::to_string(int(expected)) +
            " actual=" + std::to_string(int(voice_snapshot().phase)));
}
void action(VoiceAction value) {
    require(voice_action(value, voice_snapshot().generation), "public action rejected");
}
VoiceReceipt receipt(const char *id) { VoiceReceipt value{scope}; std::strcpy(value.id, id); return value; }
VoiceJournal persisted() {
    VoiceJournal result; require(result.load(scope), "cannot reload persisted journal"); return result;
}
void seed_pending() {
    auto journal = persisted(); require(journal.set_pending(receipt(A)), "cannot seed pending");
}
void open_reply() {
    phase(VoicePhase::Replied); action(VoiceAction::Open); phase(VoicePhase::ViewingReply);
}
void mark() {
    action(VoiceAction::MarkRead);
    require(eventually([] { return voice_snapshot().phase == VoicePhase::Idle; }, 250), "MarkRead did not return Idle in 250ms");
    require(eventually([] { return reads > 0; }), "Read barrier not reached");
    require(!voice_snapshot().unread, "locally read item still unread");
}
uint32_t record(bool boot) {
    uint32_t hold = 0;
    if (boot) action(VoiceAction::Boot);
    else { hold = voice_hold_begin(); require(hold != 0, "hold rejected while network blocked"); }
    require(eventually([] { return voice_snapshot().phase == VoicePhase::Recording; }, 250), "recording blocked by mailbox");
    return hold;
}
void cancel(uint32_t hold) {
    if (hold) voice_hold_end(hold, VoiceHoldEnd::Cancel);
    else action(VoiceAction::Discard);
    phase(VoicePhase::Idle);
}
VoiceReply exchange(VoiceRequest request, const char *id, const VoiceReadExclusions *excluded) {
    host::check_stop();
    VoiceReply reply; reply.ok = true; reply.phase = VoicePhase::Replied; reply.page_count = 1;
    if (request != VoiceRequest::Inbox) {
        require(id && std::strlen(id) == 32, "bad transport request ID"); std::strcpy(reply.request_id, id);
    }
    std::unique_lock lock(host::network_mutex);
    if (request == VoiceRequest::Read) {
        const unsigned index = ++reads;
        host::network_cv.wait(lock, [&] { return !block_read || read_permits >= index || host::stopping; });
        host::check_stop();
        reply.ok = read_success; reply.read = read_success;
        if (reply.read) server_read.emplace_back(id);
    } else if (request == VoiceRequest::Inbox) {
        const unsigned index = ++inboxes;
        last_exclusions = excluded ? excluded->count : 0;
        std::string found;
        for (const auto &candidate : inbox_ids) {
            bool skip = false;
            for (const auto &seen : server_read) skip |= seen == candidate;
            if (excluded && !ignore_exclusions)
                for (unsigned i = 0; i < excluded->count; ++i) skip |= candidate == excluded->ids[i];
            if (!skip) { found = candidate; break; }
        }
        reply.phase = found.empty() ? VoicePhase::Idle : VoicePhase::Replied;
        reply.page_count = found.empty() ? 0 : 1;
        std::strcpy(reply.request_id, found.c_str());
        host::network_cv.wait(lock, [&] { return !block_inbox || inbox_permits >= index || host::stopping; });
        host::check_stop();
    } else if (request == VoiceRequest::Upload) {
        upload_id = id; reply.phase = VoicePhase::Stored; reply.page_count = 0;
    } else if (upload_id == id) { reply.phase = VoicePhase::Stored; reply.page_count = 0; }
    return reply;
}
}
bool voice_current_scope(VoiceScope &value) { value = fixture::scope; value[0] = fixture::scope_version; return true; }
bool voice_scope_matches(const VoiceScope &value, const GadgetWifiCred &) { return value == fixture::scope; }
VoiceReply voice_exchange(VoiceRequest request, const char *id, const int16_t *, size_t,
                         const VoiceScope *scope, const VoiceReadExclusions *excluded) {
    VoiceScope current; voice_current_scope(current);
    if (scope && *scope != current) {
        VoiceReply rejected; std::strcpy(rejected.error, "scope_mismatch"); return rejected;
    }
    return fixture::exchange(request, id, excluded);
}
uint8_t *voice_download_page(const char *id, uint16_t index, uint16_t count, const VoiceScope *scope) {
    VoiceScope current; voice_current_scope(current);
    if (scope && *scope != current) return nullptr;
    ++fixture::page_downloads;
    fixture::require(std::strlen(id) == 32 && index == 0 && count == 1, "unexpected page request");
    auto *page = static_cast<uint8_t *>(std::calloc(1, VOICE_PAGE_HEADER + VOICE_PAGE_BYTES));
    std::memset(page + VOICE_PAGE_HEADER, 0xa8, VOICE_PAGE_BYTES);
    return page;
}

void voice_http_invalidate() { ++fixture::invalidations; }
VoiceResponse voice_response(const char *id, VoicePhase, const VoiceScope *scope) {
    {
        std::unique_lock lock(host::network_mutex);
        const unsigned index = ++fixture::responses;
        host::network_cv.wait(lock, [&] { return !fixture::block_response || fixture::response_permits >= index || host::stopping; });
        host::check_stop();
    }
    VoiceResponse result;
    result.reply = voice_exchange(VoiceRequest::Status, id, nullptr, 0, scope);
    if (fixture::response_ready) {
        result.reply.phase = VoicePhase::Replied; result.reply.page_count = 1;
        result.page = static_cast<uint8_t *>(std::calloc(1, VOICE_PAGE_HEADER + VOICE_PAGE_BYTES));
        std::memset(result.page + VOICE_PAGE_HEADER, 0xa8, VOICE_PAGE_BYTES);
    }
    return result;
}
