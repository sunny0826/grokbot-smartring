#include "voice_journal.h"
#include "nvs.h"
#include <algorithm>

namespace {
constexpr char KEY[] = "journal_v1";
constexpr size_t HEADER = 10, ENTRY = 48;
using Bytes = std::array<uint8_t, HEADER + ENTRY * (VOICE_READ_LIMIT + 1)>;
constexpr char HEX[] = "01234567" "89abcdef";

int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool normalize(const VoiceReceipt &input, VoiceReceipt &output)
{
    if (input.id[32] != '\0') return false;
    output.scope = input.scope;
    bool nonzero = false;
    for (size_t i = 0; i < 32; ++i) {
        const int digit = nibble(input.id[i]);
        if (digit < 0) return false;
        output.id[i] = HEX[digit];
        nonzero |= digit != 0;
    }
    output.id[32] = '\0';
    return nonzero;
}

void encode_entry(uint8_t *out, const VoiceReceipt &entry)
{
    std::copy(entry.scope.begin(), entry.scope.end(), out);
    for (size_t i = 0; i < 16; ++i)
        out[32 + i] = (nibble(entry.id[2 * i]) << 4) | nibble(entry.id[2 * i + 1]);
}

bool decode_entry(const uint8_t *in, VoiceReceipt &entry)
{
    std::copy_n(in, 32, entry.scope.begin());
    bool nonzero = false;
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t value = in[32 + i];
        entry.id[2 * i] = HEX[value >> 4];
        entry.id[2 * i + 1] = HEX[value & 15];
        nonzero |= value != 0;
    }
    entry.id[32] = '\0';
    return nonzero;
}

// VJNL, version:u8, pending:u8, count:u8, reserved:u8, total_length:u16 LE;
// optional pending, then count receipts, each scope[32] + binary request ID[16].
template <typename State>
size_t encode(const State &state, Bytes &bytes)
{
    const bool pending = state.pending.id[0] != '\0';
    const size_t length = HEADER + ENTRY * (state.count + pending);
    std::memcpy(bytes.data(), "VJNL", 4);
    bytes[4] = 1;
    bytes[5] = pending;
    bytes[6] = state.count;
    bytes[7] = 0;
    bytes[8] = length & 255;
    bytes[9] = length >> 8;
    size_t offset = HEADER;
    if (pending) {
        encode_entry(bytes.data() + offset, state.pending);
        offset += ENTRY;
    }
    for (size_t i = 0; i < state.count; ++i, offset += ENTRY)
        encode_entry(bytes.data() + offset, state.reads[i]);
    return length;
}

template <typename State>
bool decode(const Bytes &bytes, size_t length, State &state)
{
    if (length < HEADER || length > bytes.size() || std::memcmp(bytes.data(), "VJNL", 4) != 0 ||
        bytes[4] != 1 || bytes[5] > 1 || bytes[6] > VOICE_READ_LIMIT || bytes[7] != 0 ||
        size_t(bytes[8] | (bytes[9] << 8)) != length ||
        HEADER + ENTRY * (bytes[5] + bytes[6]) != length) return false;
    state = {};
    state.count = bytes[6];
    size_t offset = HEADER;
    if (bytes[5]) {
        if (!decode_entry(bytes.data() + offset, state.pending)) return false;
        offset += ENTRY;
    }
    for (size_t i = 0; i < state.count; ++i, offset += ENTRY) {
        if (!decode_entry(bytes.data() + offset, state.reads[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (state.reads[j] == state.reads[i]) return false;
    }
    return true;
}

esp_err_t read_blob(nvs_handle_t handle, Bytes &bytes, size_t &length)
{
    length = bytes.size();
    return nvs_get_blob(handle, KEY, bytes.data(), &length);
}
}

bool VoiceJournal::load(const VoiceScope &legacy_scope)
{
    ready_ = false;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open("voice", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        state_ = {};
        ready_ = true;
        return true;
    }
    if (opened != ESP_OK) return false;
    Bytes bytes{};
    size_t length;
    const esp_err_t result = read_blob(handle, bytes, length);
    State recovered{};
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        if (result != ESP_OK || !decode(bytes, length, recovered)) return false;
        state_ = recovered;
        ready_ = true;
        return true;
    }
    VoiceReceipt legacy{};
    legacy.scope = legacy_scope;
    length = sizeof(legacy.id);
    const esp_err_t old = nvs_get_str(handle, "pending", legacy.id, &length);
    nvs_close(handle);
    if (old == ESP_ERR_NVS_NOT_FOUND) {
        state_ = {};
        ready_ = true;
        return true;
    }
    if (old != ESP_OK || length != sizeof(legacy.id) ||
        !normalize(legacy, recovered.pending) || !save(recovered)) return false;
    ready_ = true;
    if (nvs_open("voice", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_erase_key(handle, "pending") == ESP_OK) (void)nvs_commit(handle);
        nvs_close(handle);
    }
    return true;
}

bool VoiceJournal::save(const State &candidate)
{
    Bytes bytes{};
    const size_t length = encode(candidate, bytes);
    nvs_handle_t handle;
    if (nvs_open("voice", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, KEY, bytes.data(), length);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        // A failed NVS update can already expose the new blob (REMOVE_FAILED).
        // Readback reconciles current NVS state; it is not a physical power-loss test.
        Bytes actual{};
        size_t actual_length = 0;
        const esp_err_t opened = nvs_open("voice", NVS_READONLY, &handle);
        esp_err_t read = opened;
        if (opened == ESP_OK) {
            read = read_blob(handle, actual, actual_length);
            nvs_close(handle);
        }
        if (read != ESP_OK || actual_length != length ||
            std::memcmp(actual.data(), bytes.data(), length) != 0) {
            const size_t old_length = encode(state_, bytes);
            const bool unchanged = (read == ESP_OK && actual_length == old_length &&
                std::memcmp(actual.data(), bytes.data(), old_length) == 0) ||
                (read == ESP_ERR_NVS_NOT_FOUND && !state_.count && !state_.pending.id[0]);
            if (!unchanged) ready_ = false;
            return false;
        }
    }
    state_ = candidate;
    ++revision_;
    return true;
}

bool VoiceJournal::set_pending(const VoiceReceipt &request)
{
    VoiceReceipt normalized{};
    if (!ready_ || !normalize(request, normalized)) return false;
    if (state_.pending == normalized) return true;
    State candidate = state_;
    candidate.pending = normalized;
    return save(candidate);
}

bool VoiceJournal::clear_pending()
{
    if (!ready_) return false;
    if (!state_.pending.id[0]) return true;
    State candidate = state_;
    candidate.pending = {};
    return save(candidate);
}

VoiceJournalResult VoiceJournal::mark_read(const VoiceReceipt &request)
{
    VoiceReceipt normalized{};
    if (!ready_ || !normalize(request, normalized)) return VoiceJournalResult::StorageError;
    const bool present = contains(normalized);
    if (present && !state_.pending.id[0]) return VoiceJournalResult::Saved;
    if (!present && state_.count == VOICE_READ_LIMIT) return VoiceJournalResult::Full;
    State candidate = state_;
    if (!present) candidate.reads[candidate.count++] = normalized;
    candidate.pending = {};
    return save(candidate) ? VoiceJournalResult::Saved : VoiceJournalResult::StorageError;
}

bool VoiceJournal::confirm_read(const VoiceReceipt &request)
{
    VoiceReceipt normalized{};
    if (!ready_ || !normalize(request, normalized)) return false;
    for (size_t i = 0; i < state_.count; ++i) {
        if (!(state_.reads[i] == normalized)) continue;
        State candidate = state_;
        for (size_t j = i + 1; j < candidate.count; ++j) candidate.reads[j - 1] = candidate.reads[j];
        candidate.reads[--candidate.count] = {};
        return save(candidate);
    }
    return true;
}

bool VoiceJournal::contains(const VoiceReceipt &request) const
{
    VoiceReceipt normalized{};
    if (!normalize(request, normalized)) return false;
    for (size_t i = 0; i < state_.count; ++i)
        if (state_.reads[i] == normalized) return true;
    return false;
}

VoiceReadExclusions VoiceJournal::exclusions(const VoiceScope &scope) const
{
    VoiceReadExclusions result{};
    for (size_t i = 0; i < state_.count; ++i)
        if (state_.reads[i].scope == scope)
            std::memcpy(result.ids[result.count++], state_.reads[i].id, sizeof(result.ids[0]));
    return result;
}
