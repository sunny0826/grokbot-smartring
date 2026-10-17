#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1, MALLOC_CAP_SPIRAM = 1;
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
struct HostQueue { std::mutex mutex; std::condition_variable cv; size_t limit, item_size; std::deque<std::vector<uint8_t>> data; };
using QueueHandle_t = HostQueue *;
using SemaphoreHandle_t = std::mutex *;
using TaskHandle_t = void *;
QueueHandle_t xQueueCreate(size_t limit, size_t item_size);
int xQueueSend(QueueHandle_t queue, const void *value, TickType_t ticks);
int xQueueReceive(QueueHandle_t queue, void *value, TickType_t ticks);
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks);
int xSemaphoreGive(SemaphoreHandle_t mutex);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t handle);
namespace host {
struct Stop {};
extern std::atomic<bool> stopping;
extern std::atomic<bool> fail_commit, fast_capture;
extern std::atomic<unsigned> commits, failed_commits, mailbox_results;
extern std::atomic<bool> pause_actor, actor_paused;
void advance_ms(int64_t milliseconds);
extern std::mutex network_mutex;
extern std::condition_variable network_cv;
void shutdown();
void seed_nvs(const std::string &name, const std::string &key, const std::string &value);
void set_nvs_file(const std::string &path);
void check_stop();
}
struct NvsHandle;
using nvs_handle_t = NvsHandle *;
int nvs_open(const char *name, int mode, nvs_handle_t *handle);
int nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
int nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size);
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size);
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size);
int nvs_erase_key(nvs_handle_t handle, const char *key);
int nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
inline int nvs_flash_init() { return ESP_OK; }
inline void *heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void *heap_caps_calloc(size_t count, size_t size, unsigned) { return std::calloc(count, size); }
inline int bsp_audio_init() { return ESP_OK; }
inline void *bsp_audio_get_codec() { return nullptr; }
inline void *bsp_audio_get_rx() { return nullptr; }
inline int esp_codec_dev_set_in_gain(void *, float) { return ESP_OK; }
int i2s_channel_read(void *, void *buffer, size_t size, size_t *read, uint32_t timeout);
uint32_t esp_random();
int64_t esp_timer_get_time();
constexpr int BOARD_BOOT_IO = 0, GPIO_MODE_INPUT = 0, GPIO_PULLUP_ENABLE = 1;
struct gpio_config_t { uint64_t pin_bit_mask; int mode, pull_up_en; };
inline int gpio_get_level(int) { return 1; }
inline int gpio_config(const gpio_config_t *) { return ESP_OK; }
