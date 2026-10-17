#include "fake_nvs.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <map>

namespace fake_nvs {
Store disk;
Faults faults;
unsigned writes, commits, erases;
struct Handle {
    nvs_open_mode_t mode;
    std::optional<std::vector<uint8_t>> staged;
    bool erase_legacy = false;
};
std::map<nvs_handle_t, Handle> handles;
nvs_handle_t next_handle = 1;

void assert_closed() { assert(handles.empty()); }
void reboot()
{
    assert_closed();
    faults = {};
}
void reset()
{
    reboot();
    disk = {};
    writes = commits = erases = 0;
}
void apply(Handle &handle)
{
    if (handle.staged) disk.blob = handle.staged;
    if (handle.erase_legacy) disk.legacy.reset();
    handle.staged.reset();
    handle.erase_legacy = false;
}
esp_err_t copy(const void *data, size_t size, void *out, size_t *length)
{
    if (!out) { *length = size; return ESP_OK; }
    if (*length < size) { *length = size; return ESP_ERR_NVS_INVALID_LENGTH; }
    std::memcpy(out, data, size);
    *length = size;
    return ESP_OK;
}
}

using namespace fake_nvs;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(std::strcmp(name, "voice") == 0);
    if (faults.fail_open) return ESP_FAIL;
    if (!disk.exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    disk.exists = true;
    *out = next_handle++;
    handles.emplace(*out, Handle{mode, std::nullopt, false});
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handles.erase(handle) == 1); }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    assert(handles.count(handle) && std::strcmp(key, "journal_v1") == 0);
    if (faults.fail_get_blob) return ESP_FAIL;
    if (disk.wrong_blob_type) return ESP_ERR_NVS_TYPE_MISMATCH;
    const auto &blob = handles.at(handle).staged ? handles.at(handle).staged : disk.blob;
    if (!blob) return ESP_ERR_NVS_NOT_FOUND;
    return copy(blob->data(), blob->size(), out, length);
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    assert(handles.count(handle) && std::strcmp(key, "pending") == 0);
    if (faults.fail_get_legacy) return ESP_FAIL;
    if (disk.wrong_legacy_type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (!disk.legacy) return ESP_ERR_NVS_NOT_FOUND;
    return copy(disk.legacy->c_str(), disk.legacy->size() + 1, out, length);
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(std::strcmp(key, "journal_v1") == 0);
    auto &target = handles.at(handle);
    assert(target.mode == NVS_READWRITE);
    ++writes;
    if (faults.fail_set && !faults.set_applies_before_failure) return ESP_FAIL;
    const auto *bytes = static_cast<const uint8_t *>(data);
    target.staged = std::vector<uint8_t>(bytes, bytes + length);
    if (faults.write_through || faults.set_applies_before_failure) apply(target);
    return faults.fail_set ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(std::strcmp(key, "pending") == 0);
    auto &target = handles.at(handle);
    assert(target.mode == NVS_READWRITE);
    ++erases;
    if (faults.fail_erase) return ESP_FAIL;
    if (!disk.legacy) return ESP_ERR_NVS_NOT_FOUND;
    target.erase_legacy = true;
    if (faults.write_through) apply(target);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    auto &target = handles.at(handle);
    assert(target.mode == NVS_READWRITE);
    ++commits;
    if (faults.fail_commit) {
        if (faults.commit_applies_before_failure) apply(target);
        if (faults.corrupt_on_failure) disk.blob = std::vector<uint8_t>{1, 2, 3};
        return ESP_FAIL;
    }
    apply(target);
    return ESP_OK;
}
