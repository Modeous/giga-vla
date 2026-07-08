// Control core: ENABLE/FAULT state machine, chunk playback with
// interpolate-from-commanded, position/velocity clamps, and the 300 ms
// comms watchdog. Pure C++ behind the time/bus seam — the caller owns the
// clock (tick(now_ms)) and the bus (reads commanded()); native tests drive
// this exact code with a simulated clock. Semantics mirror
// docs/ARCHITECTURE.md and tools/fake_giga.py.
#pragma once
#include "config/joint_limits.h"
#include "protocol.h"

namespace giga {

constexpr uint8_t kFwVer = 1;
constexpr uint32_t kTickMs = 10;           // 100 Hz control loop
constexpr uint32_t kPlaybackOffsetMs = 20;  // absorbs host jitter
constexpr uint32_t kStatePeriodMs = 50;     // 20 Hz STATE emission

class ControlCore {
 public:
  ControlCore() {
    for (uint8_t j = 0; j < kDof; ++j) commanded_[j] = 0.0f;
  }

  // Dispatch one CRC-verified payload (from FrameParser). now_ms is the
  // caller's clock at receipt.
  void on_frame(const uint8_t* payload, size_t len, uint32_t now_ms) {
    if (len == 0) return;
    if (payload[0] == kMsgEnable) {
      bool enable = false;
      if (!parse_enable(payload, len, &enable)) return;
      if (enable) {
        // Arm / clear a latched fault — but never under a pressed e-stop.
        if (!estop_) {
          armed_ = true;
          fault_latched_ = false;
          starved_ = false;
          rejected_ = false;
          has_active_ = false;
          has_admitted_since_arm_ = false;
        }
      } else {
        armed_ = false;
        has_active_ = false;
      }
    } else if (payload[0] == kMsgActionChunk) {
      if (!armed_ || fault_latched_) return;
      ActionChunk chunk;
      if (!parse_action_chunk(payload, len, &chunk)) {
        rejected_ = true;
        return;
      }
      AdmitResult result = chunk_admissible(chunk, last_seq_, commanded_);
      rejected_ = result != AdmitResult::kOk;
      if (rejected_) return;
      // Newest wins: the incoming chunk replaces the active one, and the
      // new segment starts from the currently commanded position so the
      // trajectory is continuous by construction.
      active_ = chunk;
      has_active_ = true;
      active_t0_ms_ = now_ms;
      for (uint8_t j = 0; j < kDof; ++j) seg_start_[j] = commanded_[j];
      last_seq_ = chunk.seq;
      last_admit_ms_ = now_ms;
      has_admitted_since_arm_ = true;
    }
  }

  // E-stop GPIO state (contact B). Pressing latches FAULT; recovery
  // requires release AND ENABLE(1).
  void set_estop(bool pressed) {
    estop_ = pressed;
    if (pressed) {
      fault_latched_ = true;
      armed_ = false;
      has_active_ = false;
    }
  }

  // Advance one 100 Hz control tick. Computes the interpolated target,
  // applies position and velocity clamps, and runs the comms watchdog.
  void tick(uint32_t now_ms) {
    if (armed_ && has_admitted_since_arm_ &&
        now_ms - last_admit_ms_ > kWatchdogMs) {
      // Hold, never limp: freeze commanded where it is and latch.
      fault_latched_ = true;
      starved_ = true;
      armed_ = false;
      has_active_ = false;
    }
    if (!armed_ || !has_active_) return;  // hold current commanded

    float target[kDof];
    interpolate(now_ms, target);
    for (uint8_t j = 0; j < kDof; ++j) {
      float want = target[j];
      if (want < kJointMinRad[j]) {
        want = kJointMinRad[j];
        ++clamp_count_;
      } else if (want > kJointMaxRad[j]) {
        want = kJointMaxRad[j];
        ++clamp_count_;
      }
      float max_step = kJointVmaxRadS[j] * (kTickMs / 1000.0f);
      float delta = want - commanded_[j];
      if (delta > max_step) {
        delta = max_step;
        ++clamp_count_;
      } else if (delta < -max_step) {
        delta = -max_step;
        ++clamp_count_;
      }
      commanded_[j] += delta;
    }
  }

  const float* commanded() const { return commanded_; }
  bool armed() const { return armed_; }
  bool fault_latched() const { return fault_latched_; }

  uint8_t flags() const {
    return (armed_ ? kFlagEnabled : 0) |
           (fault_latched_ ? kFlagFaultLatched : 0) |
           (starved_ ? kFlagChunkStarved : 0) |
           (rejected_ ? kFlagChunkRejected : 0) | (estop_ ? kFlagEstop : 0);
  }

  // Milestone 2 stub: q_meas echoes the commanded setpoints. Milestone 3
  // replaces this with SYNC READ measured positions.
  State snapshot(uint32_t now_ms, uint16_t crc_err_count,
                 uint8_t reset_cause) const {
    State s = {};
    s.proto_ver = kProtoVer;
    s.fw_ver = kFwVer;
    s.reset_cause = reset_cause;
    s.seq_echo = last_seq_ == kNoSeq ? 0 : last_seq_;
    s.t_ms = now_ms;
    s.flags = flags();
    s.joint_valid = (1u << kDof) - 1;
    s.crc_err_count = crc_err_count;
    s.clamp_count = clamp_count_;
    for (uint8_t j = 0; j < kDof; ++j) s.q_meas[j] = commanded_[j];
    return s;
  }

 private:
  // Waypoint k of the active chunk is scheduled at
  // t0 + kPlaybackOffsetMs + k*dt; before the first waypoint we interpolate
  // from the segment start (commanded position at admission).
  void interpolate(uint32_t now_ms, float target[kDof]) const {
    uint32_t elapsed = now_ms - active_t0_ms_;
    uint32_t dt = active_.dt_ms == 0 ? 1 : active_.dt_ms;
    if (elapsed <= kPlaybackOffsetMs) {
      float frac = static_cast<float>(elapsed) / kPlaybackOffsetMs;
      for (uint8_t j = 0; j < kDof; ++j) {
        target[j] = seg_start_[j] + (active_.q[j] - seg_start_[j]) * frac;
      }
      return;
    }
    uint32_t play = elapsed - kPlaybackOffsetMs;
    uint32_t i = play / dt;
    uint32_t last = active_.n - 1;
    if (i >= last) {  // past the end: hold the final waypoint
      for (uint8_t j = 0; j < kDof; ++j) {
        target[j] = active_.q[last * kDof + j];
      }
      return;
    }
    float frac = static_cast<float>(play - i * dt) / dt;
    for (uint8_t j = 0; j < kDof; ++j) {
      float a = active_.q[i * kDof + j];
      float b = active_.q[(i + 1) * kDof + j];
      target[j] = a + (b - a) * frac;
    }
  }

  static constexpr uint16_t kNoSeq = 0xFFFF;  // so seq 0 is "newer" at start

  float commanded_[kDof];
  float seg_start_[kDof] = {};
  ActionChunk active_ = {};
  bool has_active_ = false;
  uint32_t active_t0_ms_ = 0;
  uint32_t last_admit_ms_ = 0;
  bool has_admitted_since_arm_ = false;
  uint16_t last_seq_ = kNoSeq;
  uint16_t clamp_count_ = 0;
  bool armed_ = false;
  bool fault_latched_ = false;
  bool starved_ = false;
  bool rejected_ = false;
  bool estop_ = false;
};

}  // namespace giga
