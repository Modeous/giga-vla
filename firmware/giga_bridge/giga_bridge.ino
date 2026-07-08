// GIGA-VLA bridge — Milestone 1 scope: prove the protocol headers compile
// and parse on target. The 100 Hz control loop, chunk buffer, watchdog, and
// STATE emission are Milestone 2 (docs/ARCHITECTURE.md).
//
// Behavior: consumes frames from USB serial; the LED toggles on every
// CRC-valid frame, so a host sending chunks gets visible confirmation the
// codec agrees end-to-end.
#include "protocol.h"

static giga::FrameParser parser;
static uint32_t valid_frames = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);  // rate is nominal; native USB CDC ignores it
}

void loop() {
  while (Serial.available() > 0) {
    if (parser.feed(static_cast<uint8_t>(Serial.read()))) {
      ++valid_frames;
      digitalWrite(LED_BUILTIN, valid_frames & 1);
    }
  }
}
