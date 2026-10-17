#pragma once
#include <cstddef>
#include <cstdint>
struct TestQueue;
using QueueHandle_t = TestQueue *;
QueueHandle_t xQueueCreate(size_t, size_t);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
