#pragma once
constexpr int ESP_LOG_NONE = 0;
void esp_log_level_set(const char *, int);

#define CONFIG_LOG_DYNAMIC_LEVEL_CONTROL 1
