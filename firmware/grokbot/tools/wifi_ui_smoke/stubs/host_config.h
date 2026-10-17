#pragma once
#include <stdlib.h>
// Fail a smoke check instead of LVGL's embedded infinite-loop assertion handler.
#define LV_ASSERT_HANDLER abort();
