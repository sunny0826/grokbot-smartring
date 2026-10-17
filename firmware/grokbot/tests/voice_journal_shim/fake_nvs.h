#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fake_nvs {
struct Store {
    bool exists = false;
    std::optional<std::vector<uint8_t>> blob;
    std::optional<std::string> legacy;
    bool wrong_blob_type = false;
    bool wrong_legacy_type = false;
};
struct Faults {
    bool write_through = false;
    bool fail_open = false;
    bool fail_set = false;
    bool set_applies_before_failure = false;
    bool fail_commit = false;
    bool commit_applies_before_failure = false;
    bool corrupt_on_failure = false;
    bool fail_get_blob = false;
    bool fail_get_legacy = false;
    bool fail_erase = false;
};
extern Store disk;
extern Faults faults;
extern unsigned writes, commits, erases;
void reset();
void reboot();
void assert_closed();
}
