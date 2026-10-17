#include "voice_journal.h"
#include "fake_nvs.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nvs = fake_nvs;
using Result = VoiceJournalResult;
namespace {
unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

VoiceScope scope(uint8_t value = 1)
{
    VoiceScope result{};
    result.fill(value);
    return result;
}
VoiceReceipt receipt(unsigned value, uint8_t identity = 1)
{
    VoiceReceipt result{};
    result.scope = scope(identity);
    std::snprintf(result.id, sizeof(result.id), "%032x", value);
    return result;
}
VoiceJournal fresh()
{
    nvs::reset();
    VoiceJournal journal;
    CHECK(journal.load(scope()));
    return journal;
}
VoiceJournal restart(uint8_t legacy_identity = 1)
{
    nvs::reboot();
    VoiceJournal journal;
    CHECK(journal.load(scope(legacy_identity)));
    return journal;
}

void empty_and_encoding()
{
    nvs::reset();
    VoiceJournal unready;
    CHECK(!unready.set_pending(receipt(1)));
    CHECK(!unready.clear_pending());
    CHECK(!unready.confirm_read(receipt(1)));
    CHECK(unready.mark_read(receipt(1)) == Result::StorageError);
    auto journal = fresh();
    CHECK(journal.size() == 0 && !journal.pending().id[0]);
    CHECK(journal.clear_pending() && journal.confirm_read(receipt(1)));
    CHECK(!nvs::disk.exists && !nvs::writes && !nvs::commits && journal.revision() == 0);
    const auto request = receipt(0xab, 0x3c);
    CHECK(journal.set_pending(request));
    CHECK(journal.revision() == 1 && nvs::writes == 1 && nvs::commits == 1);
    const std::vector<uint8_t> header = {'V', 'J', 'N', 'L', 1, 1, 0, 0, 58, 0};
    const auto &blob = *nvs::disk.blob;
    CHECK(blob.size() == 58 && std::equal(header.begin(), header.end(), blob.begin()));
    CHECK(std::all_of(blob.begin() + 10, blob.begin() + 42, [](uint8_t b) { return b == 0x3c; }));
    CHECK(std::all_of(blob.begin() + 42, blob.end() - 1, [](uint8_t b) { return b == 0; }));
    CHECK(blob.back() == 0xab);
    CHECK(journal.set_pending(request) && nvs::writes == 1 && journal.revision() == 1);
    auto recovered = restart(9);
    CHECK(recovered.pending() == request);
    CHECK(recovered.clear_pending());
    CHECK(nvs::disk.blob->size() == 10);
    recovered = restart();
    CHECK(!recovered.pending().id[0]);
}

void atomic_read_and_scopes()
{
    auto journal = fresh();
    const auto a = receipt(0xab), b = receipt(2), other = receipt(0xab, 2);
    CHECK(journal.set_pending(b));
    CHECK(journal.mark_read(a) == Result::Saved);
    CHECK(journal.contains(a) && !journal.pending().id[0]);
    CHECK(nvs::disk.blob->size() == 58 && (*nvs::disk.blob)[5] == 0 && (*nvs::disk.blob)[6] == 1);
    const auto revision = journal.revision(), writes = nvs::writes;
    auto upper = a;
    upper.id[30] = 'A'; upper.id[31] = 'B';
    CHECK(journal.contains(upper));
    CHECK(journal.mark_read(upper) == Result::Saved);
    CHECK(journal.revision() == revision && nvs::writes == writes);
    CHECK(!journal.contains(other));
    CHECK(journal.mark_read(other) == Result::Saved);
    CHECK(journal.size() == 2);
    CHECK(journal.exclusions(scope()).count == 1);
    CHECK(std::strcmp(journal.exclusions(scope()).ids[0], a.id) == 0);
    CHECK(journal.exclusions(scope(2)).count == 1 && journal.exclusions(scope(3)).count == 0);
    CHECK(journal.set_pending(b));
    auto recovered = restart(3);
    CHECK(recovered.size() == 2 && recovered.contains(a) && recovered.contains(other));
    CHECK(recovered.pending() == b);
    CHECK(recovered.confirm_read(a));
    CHECK(recovered.size() == 1 && !recovered.contains(a) && recovered.contains(other));
    CHECK(recovered.pending() == b);
    recovered = restart();
    CHECK(recovered.pending() == b && recovered.contains(other) && !recovered.contains(a));
    const auto old_writes = nvs::writes, old_revision = recovered.revision();
    CHECK(recovered.confirm_read(a) && recovered.confirm_read(receipt(99)));
    CHECK(nvs::writes == old_writes && recovered.revision() == old_revision);
    CHECK(recovered.confirm_read(other));
    recovered = restart();
    CHECK(!recovered.size() && recovered.pending() == b);
}

void capacity()
{
    auto journal = fresh();
    for (unsigned i = 1; i <= VOICE_READ_LIMIT; ++i)
        CHECK(journal.mark_read(receipt(i)) == Result::Saved);
    CHECK(journal.set_pending(receipt(17)));
    CHECK(nvs::disk.blob->size() == 826);
    const auto before = *nvs::disk.blob;
    const auto rev = journal.revision(), writes = nvs::writes;
    CHECK(journal.mark_read(receipt(17)) == Result::Full);
    CHECK(journal.size() == 16 && journal.pending() == receipt(17));
    CHECK(journal.revision() == rev && nvs::writes == writes && *nvs::disk.blob == before);
    auto recovered = restart();
    CHECK(recovered.size() == 16 && recovered.pending() == receipt(17));
    for (unsigned i = 0; i < VOICE_READ_LIMIT; ++i) CHECK(recovered.at(i) == receipt(i + 1));
    CHECK(recovered.exclusions(scope()).count == 16);
    CHECK(recovered.mark_read(receipt(1)) == Result::Saved);
    CHECK(recovered.size() == 16 && !recovered.pending().id[0]);
    CHECK(recovered.confirm_read(receipt(8)));
    CHECK(recovered.mark_read(receipt(17)) == Result::Saved);
    recovered = restart();
    CHECK(recovered.size() == 16 && recovered.contains(receipt(17)) && !recovered.contains(receipt(8)));
}

void rejected_inputs()
{
    auto journal = fresh();
    CHECK(journal.set_pending(receipt(1)));
    const auto before = *nvs::disk.blob;
    std::vector<VoiceReceipt> invalid{receipt(0), receipt(2), receipt(3), receipt(4)};
    invalid[1].id[4] = 'g';
    invalid[2].id[8] = '\0';
    invalid[3].id[32] = 'f';
    for (const auto &value : invalid) {
        CHECK(!journal.set_pending(value));
        CHECK(journal.mark_read(value) == Result::StorageError);
        CHECK(!journal.confirm_read(value) && !journal.contains(value));
        CHECK(*nvs::disk.blob == before && journal.revision() == 1);
    }
}

bool mutate(VoiceJournal &journal, unsigned operation)
{
    switch (operation) {
    case 0: return journal.mark_read(receipt(2)) == Result::Saved;
    case 1: return journal.confirm_read(receipt(1));
    case 2: return journal.set_pending(receipt(3));
    default: return journal.clear_pending();
    }
}
void failures_preserve_and_retry()
{
    for (unsigned operation = 0; operation < 4; ++operation) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            auto journal = fresh();
            CHECK(journal.mark_read(receipt(1)) == Result::Saved);
            CHECK(journal.set_pending(receipt(2)));
            const auto before = *nvs::disk.blob;
            nvs::faults.fail_open = failure == 0;
            nvs::faults.fail_set = failure == 1;
            nvs::faults.fail_commit = failure == 2;
            CHECK(!mutate(journal, operation));
            CHECK(journal.revision() == 2 && journal.size() == 1 && journal.pending() == receipt(2));
            CHECK(*nvs::disk.blob == before);
            auto recovered = restart();
            CHECK(recovered.contains(receipt(1)) && recovered.pending() == receipt(2));
            CHECK(mutate(journal, operation));
            CHECK(journal.revision() == 3);
            recovered = restart();
            CHECK(recovered.size() == journal.size() && recovered.pending() == journal.pending());
            CHECK(recovered.contains(receipt(1)) == journal.contains(receipt(1)));
            CHECK(recovered.contains(receipt(2)) == journal.contains(receipt(2)));
        }
    }
    auto journal = fresh();
    nvs::faults.fail_commit = true;
    CHECK(journal.mark_read(receipt(1)) == Result::StorageError);
    CHECK(!nvs::disk.blob && !journal.size() && journal.revision() == 0);
    nvs::faults = {};
    CHECK(journal.mark_read(receipt(1)) == Result::Saved);
}

void failed_write_readback()
{
    for (unsigned operation = 0; operation < 2; ++operation) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            auto journal = fresh();
            CHECK(journal.mark_read(receipt(1)) == Result::Saved);
            CHECK(journal.set_pending(receipt(2)));
            nvs::faults.fail_set = mode == 0;
            nvs::faults.set_applies_before_failure = mode == 0;
            nvs::faults.fail_commit = mode != 0;
            nvs::faults.commit_applies_before_failure = mode == 1;
            nvs::faults.write_through = mode == 2;
            CHECK(mutate(journal, operation));
            CHECK(journal.revision() == 3);
            auto recovered = restart();
            CHECK(recovered.pending() == journal.pending() && recovered.size() == journal.size());
            CHECK(recovered.contains(receipt(1)) == journal.contains(receipt(1)));
        }
    }
}

void ambiguous_storage_freezes()
{
    for (bool corruption : {false, true}) {
        auto journal = fresh();
        CHECK(journal.set_pending(receipt(1)));
        nvs::faults.fail_commit = true;
        nvs::faults.corrupt_on_failure = corruption;
        nvs::faults.fail_get_blob = !corruption;
        CHECK(journal.mark_read(receipt(1)) == Result::StorageError);
        CHECK(journal.pending() == receipt(1) && !journal.size() && journal.revision() == 1);
        nvs::faults = {};
        const auto writes = nvs::writes;
        CHECK(!journal.set_pending(receipt(2)) && !journal.clear_pending());
        CHECK(!journal.confirm_read(receipt(1)) && journal.mark_read(receipt(2)) == Result::StorageError);
        CHECK(nvs::writes == writes);
        CHECK(journal.load(scope()) == !corruption);
        if (!corruption) CHECK(journal.mark_read(receipt(1)) == Result::Saved);
    }
}

void legacy_migration()
{
    nvs::reset();
    nvs::disk.exists = true;
    nvs::disk.legacy = receipt(0xab).id;
    nvs::faults.fail_erase = true;
    VoiceJournal journal;
    CHECK(journal.load(scope(7)) && journal.pending() == receipt(0xab, 7));
    CHECK(nvs::disk.blob && nvs::disk.legacy && nvs::erases == 1);
    auto recovered = restart(8);
    CHECK(recovered.pending() == receipt(0xab, 7));
    CHECK(recovered.mark_read(receipt(0xab, 7)) == Result::Saved);
    recovered = restart(9);
    CHECK(!recovered.pending().id[0] && recovered.contains(receipt(0xab, 7)));
    CHECK(nvs::disk.legacy.has_value());
    nvs::reset();
    nvs::disk.exists = true;
    nvs::disk.legacy = std::string(32, 'A');
    CHECK(journal.load(scope(3)));
    CHECK(std::strcmp(journal.pending().id, std::string(32, 'a').c_str()) == 0);
    CHECK(!nvs::disk.legacy && nvs::disk.blob && nvs::commits == 2);
    for (bool fail_set : {false, true}) {
        nvs::reset();
        nvs::disk.exists = true;
        nvs::disk.legacy = receipt(1).id;
        nvs::faults.fail_set = fail_set;
        nvs::faults.fail_commit = !fail_set;
        VoiceJournal failed;
        CHECK(!failed.load(scope()));
        CHECK(!nvs::disk.blob && nvs::disk.legacy && !nvs::erases);
        CHECK(!failed.clear_pending() && !failed.set_pending(receipt(2)));
        nvs::faults = {};
        CHECK(failed.load(scope()) && failed.pending() == receipt(1));
    }
}

void corrupt_recovery()
{
    auto journal = fresh();
    CHECK(journal.mark_read(receipt(1)) == Result::Saved);
    CHECK(journal.mark_read(receipt(2)) == Result::Saved);
    CHECK(journal.set_pending(receipt(3)));
    const auto good = *nvs::disk.blob;
    const auto rejects = [&](const std::vector<uint8_t> &bad) {
        nvs::reset();
        nvs::disk.exists = true;
        nvs::disk.blob = bad;
        nvs::disk.legacy = receipt(99).id;
        VoiceJournal broken;
        CHECK(!broken.load(scope()));
        CHECK(!broken.set_pending(receipt(4)) && broken.mark_read(receipt(4)) == Result::StorageError);
        CHECK(!nvs::writes && !nvs::erases && *nvs::disk.blob == bad);
    };
    for (size_t length = 0; length < good.size(); ++length)
        rejects(std::vector<uint8_t>(good.begin(), good.begin() + length));
    for (size_t field : {0, 4, 5, 6, 7, 8, 9}) {
        auto bad = good; bad[field] = 255; rejects(bad);
    }
    auto bad = good;
    std::copy_n(bad.begin() + 58, 48, bad.begin() + 106);
    rejects(bad);
    for (size_t offset : {42, 90, 138}) {
        bad = good; std::fill_n(bad.begin() + offset, 16, 0); rejects(bad);
    }
    bad = good; bad.push_back(0); rejects(bad);
    bad.resize(827); rejects(bad);
    nvs::reset();
    nvs::disk.exists = true;
    nvs::disk.wrong_blob_type = true;
    nvs::disk.legacy = receipt(1).id;
    CHECK(!journal.load(scope()) && !nvs::writes && !nvs::erases);
}

void legacy_and_load_errors()
{
    for (const auto &legacy : {std::string(), std::string(31, 'a'), std::string(33, 'a'),
                              std::string(32, 'g'), std::string(32, '0'), std::string(32, '\0')}) {
        nvs::reset();
        nvs::disk.exists = true;
        nvs::disk.legacy = legacy;
        VoiceJournal journal;
        CHECK(!journal.load(scope()) && !nvs::writes && !nvs::erases);
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        nvs::reset();
        nvs::disk.exists = true;
        nvs::faults.fail_open = failure == 0;
        nvs::faults.fail_get_blob = failure == 1;
        nvs::faults.fail_get_legacy = failure == 2;
        nvs::disk.wrong_legacy_type = failure == 3;
        VoiceJournal journal;
        CHECK(!journal.load(scope()) && !journal.set_pending(receipt(1)) && !nvs::writes);
    }
}
}

int main()
{
    const struct { const char *name; void (*run)(); } tests[] = {
        {"empty/no-op and explicit binary encoding", empty_and_encoding},
        {"atomic read, exact confirmation and scope isolation", atomic_read_and_scopes},
        {"16/17 capacity and restart", capacity},
        {"invalid IDs rejected without mutation", rejected_inputs},
        {"open/set/commit failure preserves state and permits retry", failures_preserve_and_retry},
        {"failed set/commit candidate readback", failed_write_readback},
        {"ambiguous storage blocks further mutations", ambiguous_storage_freezes},
        {"legacy migration and authoritative dual-key recovery", legacy_migration},
        {"version/length/count/duplicate/zero-ID corruption", corrupt_recovery},
        {"legacy validation and load errors", legacy_and_load_errors},
    };
    for (const auto &test : tests) {
        test.run();
        nvs::assert_closed();
        std::printf("PASS %s\n", test.name);
    }
    std::printf("PASS %zu groups, %u assertions (isolated fake NVS)\n", std::size(tests), checks);
}
