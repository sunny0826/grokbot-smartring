#pragma once
#include "nvs.h"
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 2, ESP_ERR_NVS_NEW_VERSION_FOUND = 3;
int nvs_flash_init();
int nvs_flash_erase();
