#include "voice_transport.h"
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    const char *id = "abcdef0123456789abcdef0123456789";
    auto decode = [&](const std::string &status, const std::string &count="0", const std::string &read="false") {
        std::string body = "{\"request_id\":\"" + std::string(id) + "\",\"status\":\"" + status +
            "\",\"page_count\":" + count + ",\"read\":" + read + "}";
        return voice_decode(body.c_str(), id);
    };
    assert(decode("ready").phase == VoicePhase::Waiting);
    assert(decode("claimed").phase == VoicePhase::Claimed);
    assert(decode("needs_review").phase == VoicePhase::NeedsReview);
    assert(decode("replied", "8").ok);
    assert(decode("replied", "1", "true").read);
    assert(!decode("submitted").ok);
    assert(!decode("replied", "0").ok);
    assert(!decode("replied", "9").ok);
    assert(!decode("replied", "1.5").ok);
    assert(!decode("ready", "1").ok);
    assert(!decode("ready", "0", "true").ok);
    assert(!decode("replied", "1", "1").ok);
    assert(!voice_decode("{}", id).ok);
    const char *empty = "{\"request_id\":\"\",\"status\":\"empty\",\"page_count\":0,\"read\":false}";
    assert(voice_decode(empty, nullptr).ok);
    assert(!voice_decode(empty, id).ok);
    auto unread = "{\"request_id\":\"" + std::string(id) + "\",\"status\":\"replied\",\"page_count\":2,\"read\":false}";
    assert(voice_decode(unread.c_str(), nullptr).ok);
    assert(!voice_decode(unread.c_str(), "ffffffffffffffffffffffffffffffff").ok);
    assert(!voice_decode((unread + "garbage").c_str(), nullptr).ok);
    puts("Voice protocol: real cJSON states, inbox, bounds, ownership and malformed responses PASS");
}
