// Deterministic frame-parser fuzz: 1M malformed/mutated frames must produce
// zero CRC-accepted garbage and zero hangs. Not libFuzzer — a fixed-seed
// xorshift so CI is reproducible and needs no special toolchain.
#include <cstdio>
#include <cstring>

#include "../giga_bridge/protocol.h"

using namespace giga;

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;  // fixed seed
static uint64_t xorshift() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}
static uint32_t rnd(uint32_t bound) { return xorshift() % bound; }

int main() {
  constexpr int kIterations = 1000000;
  FrameParser parser;

  // A valid reference frame to mutate.
  uint8_t payload[7 + 4 * kDof];
  payload[0] = kMsgActionChunk;
  write_u16(payload + 1, 1);
  payload[3] = kDof;
  payload[4] = 1;
  write_u16(payload + 5, 33);
  for (int j = 0; j < kDof; ++j) write_f32(payload + 7 + 4 * j, 0.01f * j);
  uint8_t valid_frame[kMaxFrame];
  size_t valid_len =
      encode_frame(payload, sizeof(payload), valid_frame, sizeof(valid_frame));

  long accepted_mutated = 0;
  long accepted_random = 0;

  for (int iter = 0; iter < kIterations; ++iter) {
    if (iter % 2 == 0) {
      // Random garbage of random length, 0x00-terminated.
      uint32_t len = 1 + rnd(64);
      for (uint32_t i = 0; i < len; ++i) {
        if (parser.feed(static_cast<uint8_t>(xorshift()))) ++accepted_random;
      }
      if (parser.feed(0)) ++accepted_random;
    } else {
      // Valid frame with a single flipped bit: CRC-16 detects every 1-bit
      // error, so none of these may ever be accepted.
      uint8_t frame[kMaxFrame];
      memcpy(frame, valid_frame, valid_len);
      uint32_t pos = rnd(static_cast<uint32_t>(valid_len - 1));  // spare the terminator
      frame[pos] ^= static_cast<uint8_t>(1 << rnd(8));
      for (size_t i = 0; i < valid_len; ++i) {
        if (parser.feed(frame[i])) ++accepted_mutated;
      }
      parser.feed(0);  // flush in case the flip killed the terminator
    }
    // Parser must still accept a pristine frame (no wedged state).
    if (iter % 10000 == 0) {
      bool ok = false;
      for (size_t i = 0; i < valid_len; ++i) ok |= parser.feed(valid_frame[i]);
      if (!ok) {
        std::printf("FAIL: parser wedged at iter %d\n", iter);
        return 1;
      }
    }
  }

  std::printf("fuzz: %d iterations, crc_err_count=%u, accepted_random=%ld, "
              "accepted_mutated=%ld\n",
              kIterations, parser.crc_err_count, accepted_random,
              accepted_mutated);
  // Random garbage can in principle contain a CRC-valid frame, but at 2^-16
  // odds on short frames we allow a tiny budget; single-bit mutations never.
  if (accepted_mutated != 0 || accepted_random > 5) {
    std::printf("FAIL: parser accepted corrupted frames\n");
    return 1;
  }
  std::printf("fuzz passed\n");
  return 0;
}
