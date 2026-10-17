#pragma once
struct bsp_battery_data_t { float voltage_v; int percent; bool charging; };
inline void bsp_battery_get_data(bsp_battery_data_t *b) { *b = {4.0f, 80, false}; }
