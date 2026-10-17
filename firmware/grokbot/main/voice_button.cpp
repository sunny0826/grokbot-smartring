#include "voice_button.h"
#include "voice_logic.h"
#include "bsp/board_config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace {
std::atomic<uint32_t> presses{0}, queued{0}, handled{0}, stage{0};
void worker(void *)
{
    VoiceButton button;
    for (;;) {
        if (button.update(gpio_get_level(BOARD_BOOT_IO) == 0, uint32_t(esp_timer_get_time() / 1000))) {
            ++presses;
            const auto snapshot = voice_snapshot();
            if (voice_action(VoiceAction::Boot, snapshot.generation)) ++queued;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}

bool voice_button_start() { return xTaskCreate(worker, "voice_button", 3072, nullptr, 3, nullptr) == pdPASS; }
void voice_button_handled() { ++handled; }
void voice_recording_stage(uint32_t value) { stage = value; }
VoiceDiagnostics voice_diagnostics()
{
    return {gpio_get_level(BOARD_BOOT_IO) == 0, presses.load(), queued.load(), handled.load(), stage.load()};
}
