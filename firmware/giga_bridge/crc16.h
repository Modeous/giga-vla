// CRC-16/CCITT-FALSE, per protocol/PROTOCOL.md. Check value:
// crc16("123456789") == 0x29B1. Pure C++ so native tests can compile it.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace giga {

inline uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace giga
