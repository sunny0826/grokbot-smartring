#include "gadget_clock.h"
#include <ctime>
#include "esp_netif_sntp.h"

bool gadget_clock_ready()
{
    const time_t now = time(nullptr);
    return now >= 1735689600LL && now < 4102444800LL; // 2025 through 2099
}

bool gadget_clock_wait()
{
    if (gadget_clock_ready()) return true;
    static bool started;
    if (!started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&config) != ESP_OK) return false;
        started = true;
    }
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    return gadget_clock_ready();
}
