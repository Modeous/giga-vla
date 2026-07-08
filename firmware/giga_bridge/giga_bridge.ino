// GIGA-VLA bridge — Milestone 2 scope: the 100 Hz deadline-paced superloop
// running the control core on target, with the servo bus stubbed to
// setpoint echo (q_meas = commanded). Milestone 3 replaces the stub with
// STS3215 SYNC WRITE/READ and wires the e-stop GPIO.
//
// All protocol and control logic lives in the headers and is tested
// natively (firmware/native_tests/); this file owns only the clock, the
// serial port, and the watchdog — the seams the tests fake.
#include <mbed.h>

#include "control.h"
#include "protocol.h"

// IWDG: conservative for bring-up so a surprise stall can't reset-loop the
// board while flashing; the decision record's ~120 ms target is re-derived
// and frozen at Milestone 3 bring-up (docs/ARCHITECTURE.md).
static constexpr uint32_t kIwdgTimeoutMs = 500;

static giga::FrameParser parser;
static giga::ControlCore core;
static uint32_t next_tick_us = 0;
static uint32_t tick_count = 0;
static uint8_t reset_cause = 2;  // 0 power-on, 1 IWDG, 2 other

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);  // rate is nominal; native USB CDC ignores it

  switch (mbed::ResetReason::get()) {
    case RESET_REASON_POWER_ON:
      reset_cause = 0;
      break;
    case RESET_REASON_WATCHDOG:
      reset_cause = 1;
      break;
    default:
      reset_cause = 2;
      break;
  }

  mbed::Watchdog::get_instance().start(kIwdgTimeoutMs);
  next_tick_us = micros() + giga::kTickMs * 1000;
}

static void emit_state() {
  giga::State s =
      core.snapshot(millis(), parser.crc_err_count, reset_cause);
  uint8_t payload[giga::kStatePayloadLen];
  giga::pack_state(s, payload, sizeof(payload));
  uint8_t frame[giga::kStatePayloadLen + 8];
  size_t frame_len =
      giga::encode_frame(payload, sizeof(payload), frame, sizeof(frame));
  // Never block the control loop on a host that stopped reading.
  if (frame_len > 0 &&
      static_cast<size_t>(Serial.availableForWrite()) >= frame_len) {
    Serial.write(frame, frame_len);
  }
}

void loop() {
  while (Serial.available() > 0) {
    if (parser.feed(static_cast<uint8_t>(Serial.read()))) {
      core.on_frame(parser.payload(), parser.payload_len(), millis());
    }
  }

  // Deadline pacing: advance by the period, never re-read "now", so the
  // tick rate has no cumulative drift (signed diff handles micros() wrap).
  if (static_cast<int32_t>(micros() - next_tick_us) >= 0) {
    next_tick_us += giga::kTickMs * 1000;
    core.tick(millis());
    mbed::Watchdog::get_instance().kick();
    if (++tick_count % (giga::kStatePeriodMs / giga::kTickMs) == 0) {
      emit_state();
      digitalWrite(LED_BUILTIN, core.armed() ? HIGH : (tick_count / 50) & 1);
    }
  }
}
