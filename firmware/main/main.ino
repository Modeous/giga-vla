// Main GIGA R1 firmware skeleton.
//
// Holds application code. The test pipeline flashes firmware/test_harness for
// hardware-in-loop runs; this sketch is here so production builds compile in CI
// alongside the test harness.

#include <Arduino.h>

#define FW_VERSION "0.1.0"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Serial.print("giga-vla main fw ");
  Serial.println(FW_VERSION);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
}
