// Native (off-target) tests for the firmware codec against the same golden
// vectors pytest uses. Run from the repo root: make -C firmware/native_tests
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../giga_bridge/protocol.h"

using namespace giga;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("FATAL: cannot open %s (run from repo root)\n", path.c_str());
    std::exit(2);
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Feed a whole file through the parser; return payloads yielded.
static std::vector<std::vector<uint8_t>> feed(FrameParser& parser,
                                              const std::vector<uint8_t>& data) {
  std::vector<std::vector<uint8_t>> payloads;
  for (uint8_t byte : data) {
    if (parser.feed(byte)) {
      payloads.emplace_back(parser.payload(),
                            parser.payload() + parser.payload_len());
    }
  }
  return payloads;
}

static const std::string kVectors = "protocol/vectors/";

static void test_crc_check_value() {
  const uint8_t digits[] = "123456789";
  CHECK(crc16_ccitt(digits, 9) == 0x29B1);
}

static void test_cobs_roundtrip() {
  const std::vector<std::vector<uint8_t>> cases = {
      {},
      {0x00},
      {0x11, 0x22, 0x00, 0x33},
      std::vector<uint8_t>(254, 0x11),
      std::vector<uint8_t>(255, 0x11),
      std::vector<uint8_t>(300, 0x00),
  };
  for (const auto& data : cases) {
    uint8_t enc[512], dec[512];
    size_t enc_len = cobs_encode(data.data(), data.size(), enc, sizeof(enc));
    CHECK(enc_len > 0);
    for (size_t i = 0; i < enc_len; ++i) CHECK(enc[i] != 0);
    size_t dec_len = cobs_decode(enc, enc_len, dec, sizeof(dec));
    CHECK(dec_len == data.size());
    CHECK(data.empty() || memcmp(dec, data.data(), data.size()) == 0);
  }
}

static void test_golden_chunk_nominal() {
  FrameParser parser;
  auto payloads = feed(parser, read_file(kVectors + "chunk_nominal.bin"));
  CHECK(payloads.size() == 1 && parser.crc_err_count == 0);
  ActionChunk chunk;
  CHECK(parse_action_chunk(payloads[0].data(), payloads[0].size(), &chunk));
  CHECK(chunk.seq == 1 && chunk.dof == 6 && chunk.n == 50 && chunk.dt_ms == 33);
  CHECK(chunk.q[0] == 0.0f);
  CHECK(chunk.q[299] == 299.0f / 1024.0f);
  const float commanded[kDof] = {0};
  CHECK(chunk_admissible(chunk, 0, commanded) == AdmitResult::kOk);

  // re-encoding reproduces the checked-in bytes exactly
  uint8_t frame[kMaxFrame];
  size_t frame_len =
      encode_frame(payloads[0].data(), payloads[0].size(), frame, sizeof(frame));
  auto golden = read_file(kVectors + "chunk_nominal.bin");
  CHECK(frame_len == golden.size());
  CHECK(memcmp(frame, golden.data(), frame_len) == 0);
}

static void test_golden_teleop_n1() {
  FrameParser parser;
  auto payloads = feed(parser, read_file(kVectors + "chunk_teleop_n1.bin"));
  CHECK(payloads.size() == 1);
  ActionChunk chunk;
  CHECK(parse_action_chunk(payloads[0].data(), payloads[0].size(), &chunk));
  CHECK(chunk.seq == 2 && chunk.n == 1);
  const float commanded[kDof] = {0};
  CHECK(chunk_admissible(chunk, 0, commanded) == AdmitResult::kOk);
}

static void test_golden_bad_crc() {
  FrameParser parser;
  auto payloads = feed(parser, read_file(kVectors + "chunk_bad_crc.bin"));
  CHECK(payloads.empty());
  CHECK(parser.crc_err_count == 1);
}

static void test_golden_nan_rejected() {
  FrameParser parser;
  auto payloads = feed(parser, read_file(kVectors + "chunk_nan.bin"));
  CHECK(payloads.size() == 1);  // framing is fine; admission must reject
  ActionChunk chunk;
  CHECK(parse_action_chunk(payloads[0].data(), payloads[0].size(), &chunk));
  const float commanded[kDof] = {0};
  CHECK(chunk_admissible(chunk, 0, commanded) == AdmitResult::kNonFinite);
}

static void test_golden_dof_mismatch_rejected() {
  FrameParser parser;
  auto payloads = feed(parser, read_file(kVectors + "chunk_dof_mismatch.bin"));
  CHECK(payloads.size() == 1);
  ActionChunk chunk;
  CHECK(parse_action_chunk(payloads[0].data(), payloads[0].size(), &chunk));
  const float commanded[kDof] = {0};
  CHECK(chunk_admissible(chunk, 0, commanded) == AdmitResult::kDofMismatch);
}

static void test_golden_enable() {
  for (const auto& [name, expected] :
       {std::pair<std::string, bool>{"enable_on.bin", true},
        {"enable_off.bin", false}}) {
    FrameParser parser;
    auto payloads = feed(parser, read_file(kVectors + name));
    CHECK(payloads.size() == 1);
    bool enable = !expected;
    CHECK(parse_enable(payloads[0].data(), payloads[0].size(), &enable));
    CHECK(enable == expected);
  }
}

static void test_golden_state_pack() {
  // pack_state must reproduce the golden STATE payload byte-for-byte.
  State s = {};
  s.proto_ver = 1;
  s.fw_ver = 1;
  s.reset_cause = 0;
  s.seq_echo = 1;
  s.t_ms = 123456;
  s.flags = 0x01;
  s.joint_valid = 0x3F;
  s.crc_err_count = 0;
  s.clamp_count = 2;
  // static_cast<float>(-j): the golden vector has +0.0 for joint 0, and
  // -0.0f would flip the sign bit and fail the byte-exact comparison.
  for (int j = 0; j < kDof; ++j) s.q_meas[j] = static_cast<float>(-j) / 1024.0f;

  uint8_t payload[kStatePayloadLen];
  CHECK(pack_state(s, payload, sizeof(payload)) == kStatePayloadLen);
  uint8_t frame[kMaxFrame];
  size_t frame_len = encode_frame(payload, sizeof(payload), frame, sizeof(frame));
  auto golden = read_file(kVectors + "state_nominal.bin");
  CHECK(frame_len == golden.size());
  CHECK(memcmp(frame, golden.data(), frame_len) == 0);
}

static void test_seq_is_newer() {
  CHECK(seq_is_newer(1, 0));
  CHECK(!seq_is_newer(0, 0));
  CHECK(!seq_is_newer(0, 1));
  CHECK(seq_is_newer(0, 0xFFFF));
  CHECK(!seq_is_newer(0xFFFF, 0));
  CHECK(seq_is_newer(0x7FFF, 0));
  CHECK(!seq_is_newer(0x8000, 0));
}

int main() {
  test_crc_check_value();
  test_cobs_roundtrip();
  test_golden_chunk_nominal();
  test_golden_teleop_n1();
  test_golden_bad_crc();
  test_golden_nan_rejected();
  test_golden_dof_mismatch_rejected();
  test_golden_enable();
  test_golden_state_pack();
  test_seq_is_newer();
  if (failures) {
    std::printf("%d FAILURES\n", failures);
    return 1;
  }
  std::printf("all native protocol tests passed\n");
  return 0;
}
