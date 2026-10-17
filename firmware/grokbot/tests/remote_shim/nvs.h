#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
int nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
int nvs_get_u16(nvs_handle_t, const char *, uint16_t *);
int nvs_set_str(nvs_handle_t, const char *, const char *);
int nvs_set_u16(nvs_handle_t, const char *, uint16_t);
int nvs_commit(nvs_handle_t);
int nvs_erase_all(nvs_handle_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_erase_key(nvs_handle_t, const char *);
