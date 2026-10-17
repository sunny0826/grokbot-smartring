#pragma once
#include "host.hpp"
#include "voice.h"
#include "voice_journal.h"
#include "voice_transport.h"
#include <functional>
#include <iostream>
#include <stdexcept>
namespace fixture {
inline constexpr char A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
inline const VoiceScope scope{1};
extern std::atomic<unsigned> reads, inboxes, read_permits, inbox_permits;
extern std::atomic<bool> block_read, block_inbox, read_success, ignore_exclusions;
extern std::atomic<unsigned> last_exclusions, scope_version;
void set_inbox(std::vector<std::string> ids);
void release_read();
void release_inbox();
std::string uploaded_id();
void require(bool condition, const char *message);
bool eventually(const std::function<bool()> &predicate, int timeout_ms = 1500);
void phase(VoicePhase expected);
void action(VoiceAction value);
VoiceReceipt receipt(const char *id);
VoiceJournal persisted();
void seed_pending();
void open_reply();
void mark();
uint32_t record(bool boot = false);
void cancel(uint32_t hold);
void run_actor_scenario(const std::string &name);
void run_ui_scenario(bool fail_save);
}
