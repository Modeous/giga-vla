// GIGA R1 hardware-in-loop test harness.
//
// Newline-terminated command protocol over USB serial.
// Each command returns a single line starting with "OK" or "ERR".
//
// Commands:
//   PING                -> OK PONG
//   VERSION             -> OK <version>
//   UPTIME              -> OK <millis>
//   LED ON|OFF|TOGGLE   -> OK <state>
//   ECHO <text>         -> OK <text>
//   ANALOG <pin>        -> OK <0..4095>
//   DIGITAL <pin>       -> OK <0|1>
//   SELFTEST            -> OK PASS or ERR <details>
//   RESET               -> OK RESETTING (then NVIC_SystemReset)

#include <Arduino.h>

#define FW_VERSION "0.1.0"
#define BAUD       115200
#define LED_PIN    LED_BUILTIN
#define BUF_LEN    128

static char buf[BUF_LEN];
static size_t blen = 0;
static bool led_state = false;

static void reply_ok(const String& s)  { Serial.print("OK ");  Serial.println(s); }
static void reply_err(const String& s) { Serial.print("ERR "); Serial.println(s); }

static String next_token(String& line) {
  line.trim();
  int sp = line.indexOf(' ');
  if (sp < 0) { String t = line; line = ""; return t; }
  String t = line.substring(0, sp);
  line = line.substring(sp + 1);
  return t;
}

static void handle(String line) {
  String cmd = next_token(line);
  cmd.toUpperCase();

  if (cmd == "PING")       { reply_ok("PONG"); }
  else if (cmd == "VERSION") { reply_ok(FW_VERSION); }
  else if (cmd == "UPTIME")  { reply_ok(String(millis())); }
  else if (cmd == "ECHO")    { reply_ok(line); }
  else if (cmd == "LED") {
    String arg = next_token(line); arg.toUpperCase();
    if (arg == "ON")          { led_state = true;  }
    else if (arg == "OFF")    { led_state = false; }
    else if (arg == "TOGGLE") { led_state = !led_state; }
    else { reply_err("LED arg must be ON|OFF|TOGGLE"); return; }
    digitalWrite(LED_PIN, led_state ? HIGH : LOW);
    reply_ok(led_state ? "ON" : "OFF");
  }
  else if (cmd == "ANALOG") {
    int pin = next_token(line).toInt();
    reply_ok(String(analogRead(pin)));
  }
  else if (cmd == "DIGITAL") {
    int pin = next_token(line).toInt();
    pinMode(pin, INPUT);
    reply_ok(String(digitalRead(pin)));
  }
  else if (cmd == "SELFTEST") {
    // Minimal self-test: toggle LED, read analog, confirm millis advances.
    unsigned long t0 = millis();
    digitalWrite(LED_PIN, HIGH);
    delay(2);
    int a = analogRead(A0);
    digitalWrite(LED_PIN, led_state ? HIGH : LOW);
    unsigned long dt = millis() - t0;
    if (dt < 1) { reply_err("clock"); return; }
    if (a < 0 || a > 4095) { reply_err("analog out of range"); return; }
    reply_ok("PASS");
  }
  else if (cmd == "RESET") {
    reply_ok("RESETTING");
    Serial.flush();
    delay(50);
    NVIC_SystemReset();
  }
  else if (cmd.length() == 0) {
    // Tolerate empty lines.
  }
  else {
    reply_err("unknown: " + cmd);
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.begin(BAUD);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { /* wait briefly for host */ }
  Serial.println("READY " FW_VERSION);
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[blen] = '\0';
      handle(String(buf));
      blen = 0;
      continue;
    }
    if (blen < BUF_LEN - 1) buf[blen++] = c;
    else { blen = 0; reply_err("overflow"); }
  }
}
