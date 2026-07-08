// COBS encode/decode, per protocol/PROTOCOL.md. Pure C++ (no Arduino
// includes) so the native test suite compiles these exact bytes-on-the-wire
// routines off-target.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace giga {

// Encodes src[0..len) into dst; returns encoded length, or 0 if dst_cap is
// too small. Worst case output: len + len/254 + 1 bytes.
inline size_t cobs_encode(const uint8_t* src, size_t len, uint8_t* dst,
                          size_t dst_cap) {
  size_t out = 0;
  size_t code_pos = 0;
  uint8_t code = 1;
  if (dst_cap == 0) return 0;
  out = 1;  // reserve first code byte
  for (size_t i = 0; i < len; ++i) {
    if (src[i] == 0) {
      dst[code_pos] = code;
      code_pos = out;
      if (out >= dst_cap) return 0;
      dst[out++] = 0;  // placeholder for next code byte
      code = 1;
    } else {
      if (out >= dst_cap) return 0;
      dst[out++] = src[i];
      if (++code == 0xFF) {
        dst[code_pos] = code;
        code_pos = out;
        if (out >= dst_cap) return 0;
        dst[out++] = 0;
        code = 1;
      }
    }
  }
  dst[code_pos] = code;
  return out;
}

// Decodes src[0..len) (one frame, no 0x00 inside) into dst; returns decoded
// length, or SIZE_MAX on malformed input or overflow.
inline size_t cobs_decode(const uint8_t* src, size_t len, uint8_t* dst,
                          size_t dst_cap) {
  size_t out = 0;
  size_t i = 0;
  while (i < len) {
    uint8_t code = src[i];
    if (code == 0 || i + code > len) return SIZE_MAX;
    for (uint8_t k = 1; k < code; ++k) {
      if (out >= dst_cap) return SIZE_MAX;
      dst[out++] = src[i + k];
    }
    i += code;
    if (code < 0xFF && i < len) {
      if (out >= dst_cap) return SIZE_MAX;
      dst[out++] = 0;
    }
  }
  return out;
}

}  // namespace giga
