#include "host.hpp"
#include <fstream>
#include <iomanip>
namespace {
std::mutex tasks_mutex;
thread_local std::string task_name;
std::vector<std::thread> tasks;
std::vector<HostQueue *> queues;
std::exception_ptr task_error;
std::atomic<int64_t> time_offset{0};
std::mutex nvs_mutex;
std::map<std::string, std::map<std::string, std::string>> durable;
std::string nvs_path;
const auto epoch = std::chrono::steady_clock::now();
void persist() {
    if (nvs_path.empty()) return;
    std::ofstream out(nvs_path);
    for (const auto &[ns, values] : durable)
        for (const auto &[key, value] : values)
            out << std::quoted(ns) << ' ' << std::quoted(key) << ' ' << std::quoted(value) << '\n';
}
}
namespace host {
std::atomic<bool> stopping{false}, fail_commit{false}, fast_capture{false};
std::atomic<unsigned> commits{0}, failed_commits{0}, mailbox_results{0};
std::atomic<bool> pause_actor{false}, actor_paused{false};
void advance_ms(int64_t milliseconds) { time_offset += milliseconds * 1000; }
std::mutex network_mutex;
std::condition_variable network_cv;
void check_stop() { if (stopping) throw Stop{}; }
void shutdown() {
    stopping = true;
    network_cv.notify_all();
    for (auto *queue : queues) queue->cv.notify_all();
    std::vector<std::thread> owned;
    { std::lock_guard lock(tasks_mutex); owned.swap(tasks); }
    for (auto &task : owned) if (task.joinable()) task.join();
    if (task_error) std::rethrow_exception(task_error);
}
void seed_nvs(const std::string &name, const std::string &key, const std::string &value) {
    std::lock_guard lock(nvs_mutex); durable[name][key] = value; persist();
}
void set_nvs_file(const std::string &path) {
    std::lock_guard lock(nvs_mutex); nvs_path = path;
    std::ifstream in(path);
    std::string ns, key, value;
    while (in >> std::quoted(ns) >> std::quoted(key) >> std::quoted(value)) durable[ns][key] = value;
}
}
QueueHandle_t xQueueCreate(size_t limit, size_t item_size) {
    auto *queue = new HostQueue; queue->limit = limit; queue->item_size = item_size;
    queues.push_back(queue); return queue;
}
int xQueueSend(QueueHandle_t queue, const void *value, TickType_t ticks) {
    std::unique_lock lock(queue->mutex);
    auto ready = [&] { return queue->data.size() < queue->limit || host::stopping.load(); };
    if (ticks == portMAX_DELAY) queue->cv.wait(lock, ready);
    else if (ticks) queue->cv.wait_for(lock, std::chrono::milliseconds(ticks), ready);
    if (ticks) host::check_stop();
    if (queue->data.size() == queue->limit) return pdFALSE;
    const auto *bytes = static_cast<const uint8_t *>(value);
    queue->data.emplace_back(bytes, bytes + queue->item_size);
    if (task_name == "voice_mailbox") ++host::mailbox_results;
    queue->cv.notify_all(); return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *value, TickType_t ticks) {
    std::unique_lock lock(queue->mutex);
    auto ready = [&] { return !queue->data.empty() || host::stopping.load(); };
    if (ticks == portMAX_DELAY) queue->cv.wait(lock, ready);
    else if (ticks) queue->cv.wait_for(lock, std::chrono::milliseconds(ticks), ready);
    if (ticks) host::check_stop();
    if (queue->data.empty()) return pdFALSE;
    std::memcpy(value, queue->data.front().data(), queue->item_size);
    queue->data.pop_front(); queue->cv.notify_all(); return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
    std::lock_guard lock(queue->mutex); return queue->data.size();
}
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) { mutex->lock(); return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); return pdTRUE; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned, void *arg, unsigned, TaskHandle_t *) {
    std::lock_guard lock(tasks_mutex);
    tasks.emplace_back([fn, arg, name = std::string(name)] {
        task_name = name;
        try { fn(arg); throw std::runtime_error("FreeRTOS task returned"); } catch (const host::Stop &) {}
        catch (...) { std::lock_guard error_lock(tasks_mutex); task_error = std::current_exception(); }
    }); return pdPASS;
}
void vTaskDelete(TaskHandle_t handle) { assert(!handle); throw host::Stop{}; }
void vTaskDelay(TickType_t ticks) {
    host::check_stop();
    std::unique_lock lock(host::network_mutex);
    if (task_name == "voice" && ticks == 30 && host::pause_actor) {
        host::actor_paused = true;
        host::network_cv.wait(lock, [] { return !host::pause_actor || host::stopping; });
        host::actor_paused = false;
    }
    host::network_cv.wait_for(lock, std::chrono::milliseconds(ticks), [] { return host::stopping.load(); });
    host::check_stop();
}
struct NvsHandle {
    std::string name;
    std::map<std::string, std::string> values, changes;
    std::vector<std::string> erased;
};
int nvs_open(const char *name, int, nvs_handle_t *handle) {
    std::lock_guard lock(nvs_mutex);
    *handle = new NvsHandle{name, durable[name], {}, {}}; return ESP_OK;
}
int nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    handle->values[key] = value; handle->changes[key] = value; return ESP_OK;
}
int nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size) {
    const auto entry = handle->values.find(key);
    if (entry == handle->values.end()) return ESP_ERR_NVS_NOT_FOUND;
    const auto length = entry->second.size() + 1;
    if (!value) { *size = length; return ESP_OK; }
    if (*size < length) return ESP_FAIL;
    std::memcpy(value, entry->second.c_str(), length); *size = length; return ESP_OK;
}
int nvs_erase_key(nvs_handle_t handle, const char *key) {
    if (!handle->values.erase(key)) return ESP_ERR_NVS_NOT_FOUND;
    handle->changes.erase(key); handle->erased.emplace_back(key); return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    if (host::fail_commit) { ++host::failed_commits; return ESP_FAIL; }
    ++host::commits;
    std::lock_guard lock(nvs_mutex);
    auto &values = durable[handle->name];
    for (const auto &key : handle->erased) values.erase(key);
    for (const auto &[key, value] : handle->changes) values[key] = value;
    handle->erased.clear(); handle->changes.clear(); persist(); return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    std::string bytes(static_cast<const char *>(value), size);
    handle->values[key] = bytes; handle->changes[key] = bytes; return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size) {
    const auto entry = handle->values.find(key);
    if (entry == handle->values.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!value) { *size = entry->second.size(); return ESP_OK; }
    if (*size < entry->second.size()) return ESP_FAIL;
    std::memcpy(value, entry->second.data(), entry->second.size());
    *size = entry->second.size(); return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { delete handle; }
int i2s_channel_read(void *, void *buffer, size_t size, size_t *read, uint32_t timeout) {
    if (!timeout) { *read = 0; return ESP_OK; }
    vTaskDelay(host::fast_capture ? 0 : 20);
    auto *samples = static_cast<int16_t *>(buffer);
    for (size_t i = 0; i < size / 2; ++i) samples[i] = 256;
    *read = size; return ESP_OK;
}
uint32_t esp_random() { static std::atomic<uint32_t> next{123}; return ++next; }
int64_t esp_timer_get_time() {
    return time_offset + std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch).count();
}
