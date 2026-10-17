#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
constexpr uint16_t VOICE_PAGE_WIDTH = 272;
constexpr uint16_t VOICE_PAGE_HEIGHT = 168;
constexpr uint16_t VOICE_PAGE_LIMIT = 8;
constexpr size_t VOICE_PAGE_BYTES = VOICE_PAGE_WIDTH * VOICE_PAGE_HEIGHT * 2;
constexpr size_t VOICE_PAGE_HEADER = 16;
inline uint32_t voice_page_le(const uint8_t *p, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= uint32_t(p[i]) << (8 * i);
    return value;
}
inline bool voice_page_valid(const uint8_t *data, size_t size, uint16_t index, uint16_t count) {
    if (size != VOICE_PAGE_HEADER + VOICE_PAGE_BYTES || !count || count > VOICE_PAGE_LIMIT ||
        index >= count || memcmp(data, "VPG1", 4) ||
        voice_page_le(data + 4, 2) != VOICE_PAGE_WIDTH ||
        voice_page_le(data + 6, 2) != VOICE_PAGE_HEIGHT ||
        voice_page_le(data + 8, 2) != index || voice_page_le(data + 10, 2) != count) return false;
    uint32_t crc = 0xffffffff;
    for (size_t i = VOICE_PAGE_HEADER; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return (crc ^ 0xffffffffu) == voice_page_le(data + 12, 4);
}
