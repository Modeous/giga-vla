// Wire protocol: frame parser, message parse/encode, chunk admission.
// Mirrors host/giga_host/{framing,messages}.py — a change here is a protocol
// change and must land in one PR with the host codec, PROTOCOL.md, and the
// golden vectors. Pure C++ (no Arduino includes): the native test suite and
// fuzzer compile this file exactly as the target does.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cobs.h"
#include "crc16.h"

namespace giga {

constexpr uint8_t kProtoVer = 1;
constexpr uint8_t kDof = 6;
constexpr uint8_t kMaxSteps = 100;
constexpr size_t kMaxFrame = 2048;
constexpr uint32_t kWatchdogMs = 300;
constexpr float kMaxFirstStepRad = 0.261799f;  // 15 degrees

constexpr uint8_t kMsgActionChunk = 0x01;
constexpr uint8_t kMsgEnable = 0x02;
constexpr uint8_t kMsgState = 0x10;

constexpr uint8_t kFlagEnabled = 1 << 0;
constexpr uint8_t kFlagFaultLatched = 1 << 1;
constexpr uint8_t kFlagChunkStarved = 1 << 2;
constexpr uint8_t kFlagChunkRejected = 1 << 3;
constexpr uint8_t kFlagEstop = 1 << 4;

// --- little-endian field helpers (target and test hosts are both LE, but
// going through memcpy keeps this alignment-safe) ---
inline uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline float read_f32(const uint8_t* p) {
  uint32_t bits = read_u32(p);
  float f;
  memcpy(&f, &bits, 4);
  return f;
}
inline void write_u16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
inline void write_u32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
inline void write_f32(uint8_t* p, float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  write_u32(p, bits);
}

// --- framing ---

// Feed bytes one at a time; returns true when a CRC-verified payload (CRC
// stripped) is ready in payload()/payload_len(). Corrupt frames increment
// crc_err_count and are dropped; the stream resyncs at the next 0x00.
class FrameParser {
 public:
  bool feed(uint8_t byte) {
    if (byte != 0) {
      if (len_ >= kMaxFrame) {
        overflow_ = true;
      } else {
        buf_[len_++] = byte;
      }
      return false;
    }
    size_t encoded_len = len_;
    bool overflowed = overflow_;
    len_ = 0;
    overflow_ = false;
    if (encoded_len == 0) return false;  // idle delimiter
    if (overflowed) {
      ++crc_err_count;
      return false;
    }
    size_t decoded = cobs_decode(buf_, encoded_len, decoded_, sizeof(decoded_));
    if (decoded == SIZE_MAX || decoded < 3 ||
        crc16_ccitt(decoded_, decoded - 2) != read_u16(decoded_ + decoded - 2)) {
      ++crc_err_count;
      return false;
    }
    payload_len_ = decoded - 2;
    return true;
  }

  const uint8_t* payload() const { return decoded_; }
  size_t payload_len() const { return payload_len_; }

  uint16_t crc_err_count = 0;

 private:
  uint8_t buf_[kMaxFrame];
  uint8_t decoded_[kMaxFrame];
  size_t len_ = 0;
  size_t payload_len_ = 0;
  bool overflow_ = false;
};

// payload (without CRC) -> full wire frame incl. 0x00 terminator.
// Returns frame length, or 0 if dst_cap is too small.
inline size_t encode_frame(const uint8_t* payload, size_t len, uint8_t* dst,
                           size_t dst_cap) {
  uint8_t staged[kMaxFrame];
  if (len + 2 > sizeof(staged)) return 0;
  memcpy(staged, payload, len);
  write_u16(staged + len, crc16_ccitt(payload, len));
  size_t encoded = cobs_encode(staged, len + 2, dst, dst_cap);
  if (encoded == 0 || encoded + 1 > dst_cap) return 0;
  dst[encoded] = 0;
  return encoded + 1;
}

// --- messages ---

struct ActionChunk {
  uint16_t seq;
  uint8_t dof;
  uint8_t n;
  uint16_t dt_ms;
  float q[kMaxSteps * kDof];  // step-major
};

// Structural parse only (admission is separate). Rejects payloads whose
// length disagrees with n*dof or that would overflow the q buffer.
inline bool parse_action_chunk(const uint8_t* p, size_t len, ActionChunk* out) {
  if (len < 7 || p[0] != kMsgActionChunk) return false;
  out->seq = read_u16(p + 1);
  out->dof = p[3];
  out->n = p[4];
  out->dt_ms = read_u16(p + 5);
  size_t count = static_cast<size_t>(out->n) * out->dof;
  if (out->dof == 0 || out->n == 0 || count > kMaxSteps * kDof ||
      len != 7 + 4 * count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) out->q[i] = read_f32(p + 7 + 4 * i);
  return true;
}

inline bool parse_enable(const uint8_t* p, size_t len, bool* enable) {
  if (len != 2 || p[0] != kMsgEnable) return false;
  *enable = p[1] != 0;
  return true;
}

// Serial-number arithmetic on u16: newer iff (seq-last) mod 2^16 in 1..0x7FFF.
inline bool seq_is_newer(uint16_t seq, uint16_t last) {
  uint16_t delta = static_cast<uint16_t>(seq - last);
  return delta != 0 && delta < 0x8000;
}

enum class AdmitResult : uint8_t {
  kOk,
  kDofMismatch,
  kBadStepCount,
  kNonFinite,
  kStaleSeq,
  kDiscontinuity,
};

// The admission rules from PROTOCOL.md; mirrored by host messages.py.
inline AdmitResult chunk_admissible(const ActionChunk& chunk, uint16_t last_seq,
                                    const float commanded[kDof]) {
  if (chunk.dof != kDof) return AdmitResult::kDofMismatch;
  if (chunk.n < 1 || chunk.n > kMaxSteps) return AdmitResult::kBadStepCount;
  size_t count = static_cast<size_t>(chunk.n) * chunk.dof;
  for (size_t i = 0; i < count; ++i) {
    if (!isfinite(chunk.q[i])) return AdmitResult::kNonFinite;
  }
  if (!seq_is_newer(chunk.seq, last_seq)) return AdmitResult::kStaleSeq;
  for (uint8_t j = 0; j < kDof; ++j) {
    if (fabsf(chunk.q[j] - commanded[j]) > kMaxFirstStepRad) {
      return AdmitResult::kDiscontinuity;
    }
  }
  return AdmitResult::kOk;
}

struct State {
  uint8_t proto_ver;
  uint8_t fw_ver;
  uint8_t reset_cause;
  uint16_t seq_echo;
  uint32_t t_ms;
  uint8_t flags;
  uint8_t joint_valid;
  uint16_t crc_err_count;
  uint16_t clamp_count;
  float q_meas[kDof];
};

constexpr size_t kStatePayloadLen = 16 + 4 * kDof;  // without CRC

inline size_t pack_state(const State& s, uint8_t* dst, size_t dst_cap) {
  if (dst_cap < kStatePayloadLen) return 0;
  dst[0] = kMsgState;
  dst[1] = s.proto_ver;
  dst[2] = s.fw_ver;
  dst[3] = s.reset_cause;
  write_u16(dst + 4, s.seq_echo);
  write_u32(dst + 6, s.t_ms);
  dst[10] = s.flags;
  dst[11] = s.joint_valid;
  write_u16(dst + 12, s.crc_err_count);
  write_u16(dst + 14, s.clamp_count);
  for (uint8_t j = 0; j < kDof; ++j) write_f32(dst + 16 + 4 * j, s.q_meas[j]);
  return kStatePayloadLen;
}

}  // namespace giga
