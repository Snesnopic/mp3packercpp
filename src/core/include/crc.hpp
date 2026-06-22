#pragma once
#include <cstdint>
#include <cstddef>

class Crc {
public:
    static uint16_t calc_mpeg_crc(const uint8_t* data, size_t length, uint16_t init_crc = 0xFFFF);
    static uint16_t calc_lame_crc(const uint8_t* data, size_t length, uint16_t init_crc = 0x0000);
};
