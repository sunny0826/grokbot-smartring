#pragma once
#include <array>
#include <cstdint>
#include <cstring>

constexpr size_t VOICE_READ_LIMIT = 16;
using VoiceScope = std::array<uint8_t, 32>;
struct VoiceReceipt {
    VoiceScope scope{};
    char id[33]{};
    bool operator==(const VoiceReceipt &other) const {
        return scope == other.scope && std::strcmp(id, other.id) == 0;
    }
};
struct VoiceReadExclusions {
    uint8_t count = 0;
    char ids[VOICE_READ_LIMIT][33]{};
};

