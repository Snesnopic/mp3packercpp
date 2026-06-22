#include "crc.hpp"

uint16_t Crc::calc_mpeg_crc(const uint8_t* data, size_t length, uint16_t init_crc) {
    uint16_t crc = init_crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (static_cast<uint16_t>(data[i]) << 8);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x8005;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

uint16_t Crc::calc_lame_crc(const uint8_t* data, size_t length, uint16_t init_crc) {
    uint16_t crc = init_crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (static_cast<uint16_t>(data[i]) << 8);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x8005;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
