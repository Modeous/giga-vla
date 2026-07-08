// Native tests for the control core: state machine, chunk playback
// continuity, clamps, watchdog, e-stop. Deterministic simulated clock —
// this is the Milestone 2 "state machine and timers tested natively behind
// the seam" gate (docs/ARCHITECTURE.md).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../giga_bridge/control.h"

using namespace giga;

static int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

// --- helpers: build raw payloads the way the host codec would ---

static std::vector<uint8_t> enable_payload(bool enable) {
  return {kMsgEnable, static_cast<uint8_t>(enable ? 1 : 0)};
}

static std::vector<uint8_t> chunk_payload(uint16_t seq, uint8_t dof, uint8_t n,
                                          uint16_t dt_ms,
                                          const std::vector<float>& q) {
  std::vector<uint8_t> p(7 + 4 * q.size());
  p[0] = kMsgActionChunk;
  write_u16(p.data() + 1, seq);
  p[3] = dof;
  p[4] = n;
  write_u16(p.data() + 5, dt_ms);
  for (size_t i = 0; i < q.size(); ++i) write_f32(p.data() + 7 + 4 * i, q[i]);
  return p;
}

// A chunk holding every joint at `value` for n steps.
static std::vector<uint8_t> flat_chunk(uint16_t seq, float value, uint8_t n = 5,
                                       uint16_t dt_ms = 33) {
  std::vector<float> q(static_cast<size_t>(n) * kDof, value);
  return chunk_payload(seq, kDof, n, dt_ms, q);
}

static void send(ControlCore& core, const std::vector<uint8_t>& p,
                 uint32_t now_ms) {
  core.on_frame(p.data(), p.size(), now_ms);
}

// Run ticks from `from_ms` (exclusive) to `to_ms` (inclusive) at kTickMs,
// asserting per-tick continuity against the velocity clamp.
static uint32_t run_ticks(ControlCore& core, uint32_t from_ms, uint32_t to_ms) {
  float prev[kDof];
  memcpy(prev, core.commanded(), sizeof(prev));
  for (uint32_t t = from_ms + kTickMs; t <= to_ms; t += kTickMs) {
    core.tick(t);
    for (uint8_t j = 0; j < kDof; ++j) {
      float step = std::fabs(core.commanded()[j] - prev[j]);
      float bound = kJointVmaxRadS[j] * (kTickMs / 1000.0f) + 1e-6f;
      CHECK(step <= bound);
      prev[j] = core.commanded()[j];
    }
  }
  return to_ms;
}

static void test_boot_disarmed_ignores_chunks() {
  ControlCore core;
  CHECK(!core.armed());
  send(core, flat_chunk(1, 0.1f), 0);
  run_ticks(core, 0, 100);
  CHECK(core.commanded()[0] == 0.0f);
  CHECK(core.snapshot(100, 0, 0).seq_echo == 0);
}

static void test_enable_admit_track() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  CHECK(core.armed());
  send(core, flat_chunk(1, 0.2f, 5, 33), 0);
  // 5 steps at 33 ms + 20 ms offset = fully played by ~185 ms; give margin.
  run_ticks(core, 0, 300);
  for (uint8_t j = 0; j < kDof; ++j) {
    CHECK(std::fabs(core.commanded()[j] - 0.2f) < 1e-3f);
  }
  State s = core.snapshot(300, 0, 0);
  CHECK(s.seq_echo == 1);
  CHECK(s.flags == kFlagEnabled);
}

static void test_chunk_swap_is_continuous() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  send(core, flat_chunk(1, 0.2f), 0);
  uint32_t t = run_ticks(core, 0, 80);  // mid-playback
  // New chunk toward a different target; must remain continuous (asserted
  // inside run_ticks) and converge on the new target.
  float here = core.commanded()[0];
  std::vector<float> q(5 * kDof, here - 0.1f);
  send(core, chunk_payload(2, kDof, 5, 33, q), t);
  run_ticks(core, t, t + 400);
  for (uint8_t j = 0; j < kDof; ++j) {
    CHECK(std::fabs(core.commanded()[j] - (here - 0.1f)) < 1e-3f);
  }
}

static void test_teleport_rejected() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  send(core, flat_chunk(1, 1.0f), 0);  // > 15 degrees from commanded 0
  State s = core.snapshot(0, 0, 0);
  CHECK(s.flags & kFlagChunkRejected);
  CHECK(s.seq_echo == 0);
  run_ticks(core, 0, 100);
  CHECK(core.commanded()[0] == 0.0f);  // did not move
  // A sane follow-up chunk is accepted and clears the rejected flag.
  send(core, flat_chunk(2, 0.1f), 100);
  CHECK(!(core.snapshot(100, 0, 0).flags & kFlagChunkRejected));
}

static void test_stale_seq_rejected() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  send(core, flat_chunk(5, 0.1f), 0);
  CHECK(core.snapshot(0, 0, 0).seq_echo == 5);
  send(core, flat_chunk(5, 0.2f), 10);  // same seq: stale
  CHECK(core.snapshot(10, 0, 0).flags & kFlagChunkRejected);
  CHECK(core.snapshot(10, 0, 0).seq_echo == 5);
}

static void test_watchdog_holds_latches_no_auto_resume() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  send(core, flat_chunk(1, 0.1f), 0);
  uint32_t t = run_ticks(core, 0, 200);
  float held[kDof];
  memcpy(held, core.commanded(), sizeof(held));
  // Starve past the watchdog: latch FAULT, hold position.
  t = run_ticks(core, t, t + kWatchdogMs + 100);
  State s = core.snapshot(t, 0, 0);
  CHECK(s.flags & kFlagFaultLatched);
  CHECK(s.flags & kFlagChunkStarved);
  CHECK(!(s.flags & kFlagEnabled));
  for (uint8_t j = 0; j < kDof; ++j) {
    CHECK(core.commanded()[j] == held[j]);
  }
  // Fresh chunk must NOT clear the fault (no auto-resume).
  send(core, flat_chunk(2, 0.12f), t);
  CHECK(core.snapshot(t, 0, 0).flags & kFlagFaultLatched);
  CHECK(core.snapshot(t, 0, 0).seq_echo == 1);
  // Explicit ENABLE(1) clears it; the next chunk plays.
  send(core, enable_payload(true), t);
  CHECK(!core.fault_latched());
  send(core, flat_chunk(3, 0.12f), t);
  run_ticks(core, t, t + 300);
  CHECK(std::fabs(core.commanded()[0] - 0.12f) < 1e-3f);
}

static void test_watchdog_not_armed_before_first_chunk() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  run_ticks(core, 0, 1000);  // enabled, host quiet: no chunks yet, no fault
  CHECK(!core.fault_latched());
  CHECK(core.armed());
}

static void test_estop_latches_and_gates_enable() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  send(core, flat_chunk(1, 0.1f), 0);
  core.set_estop(true);
  State s = core.snapshot(0, 0, 0);
  CHECK((s.flags & kFlagEstop) && (s.flags & kFlagFaultLatched));
  CHECK(!(s.flags & kFlagEnabled));
  // ENABLE while pressed is refused.
  send(core, enable_payload(true), 10);
  CHECK(!core.armed());
  // Released + ENABLE(1) recovers.
  core.set_estop(false);
  send(core, enable_payload(true), 20);
  CHECK(core.armed() && !core.fault_latched());
}

static void test_position_limit_clamped_not_faulted() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  // Walk joint 0 to near its max with successive small chunks.
  uint32_t t = 0;
  uint16_t seq = 0;
  float pos = 0.0f;
  while (pos < kJointMaxRad[0] - 0.1f) {
    pos += 0.2f;
    std::vector<float> q(5 * kDof, 0.0f);
    for (int step = 0; step < 5; ++step) q[step * kDof] = pos;
    send(core, chunk_payload(++seq, kDof, 5, 33, q), t);
    t = run_ticks(core, t, t + 250);
  }
  uint16_t clamps_before = core.snapshot(t, 0, 0).clamp_count;
  // Command slightly past the limit: graze is clamped, not faulted.
  std::vector<float> q(5 * kDof, 0.0f);
  for (int step = 0; step < 5; ++step) q[step * kDof] = kJointMaxRad[0] + 0.05f;
  send(core, chunk_payload(++seq, kDof, 5, 33, q), t);
  CHECK(!(core.snapshot(t, 0, 0).flags & kFlagChunkRejected));
  t = run_ticks(core, t, t + 300);
  CHECK(core.commanded()[0] <= kJointMaxRad[0] + 1e-6f);
  CHECK(std::fabs(core.commanded()[0] - kJointMaxRad[0]) < 1e-3f);
  CHECK(core.snapshot(t, 0, 0).clamp_count > clamps_before);
  CHECK(!core.fault_latched());
}

static void test_velocity_clamp_counts() {
  ControlCore core;
  send(core, enable_payload(true), 0);
  // First step 0.25 rad away (admissible, < 15 deg) in one 33 ms step:
  // demands ~7.6 rad/s, well over vmax — must be rate-limited and counted.
  send(core, flat_chunk(1, 0.25f, 1, 33), 0);
  run_ticks(core, 0, 200);  // continuity bound asserted inside
  CHECK(std::fabs(core.commanded()[0] - 0.25f) < 1e-3f);
  CHECK(core.snapshot(200, 0, 0).clamp_count > 0);
}

int main() {
  test_boot_disarmed_ignores_chunks();
  test_enable_admit_track();
  test_chunk_swap_is_continuous();
  test_teleport_rejected();
  test_stale_seq_rejected();
  test_watchdog_holds_latches_no_auto_resume();
  test_watchdog_not_armed_before_first_chunk();
  test_estop_latches_and_gates_enable();
  test_position_limit_clamped_not_faulted();
  test_velocity_clamp_counts();
  if (failures) {
    std::printf("%d FAILURES\n", failures);
    return 1;
  }
  std::printf("all native control tests passed\n");
  return 0;
}
