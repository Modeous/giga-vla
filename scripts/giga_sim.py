#!/usr/bin/env python3
"""Software emulator of the GIGA R1 test harness, over a PTY (Unix only).

Lets the host-side pipeline (scripts/detect_giga.py consumers, the pytest HIL
suite) be exercised without a physical board. It mirrors the command protocol
implemented in firmware/test_harness/test_harness.ino exactly, so a green run
against this emulator validates everything except the board itself.

Usage:
  giga_sim.py                 # serve until killed; prints the slave device path
  giga_sim.py --port-file P   # also write the slave path to file P

The printed path can be passed to the suite via GIGA_PORT.
"""

from __future__ import annotations

import argparse
import os
import pty
import sys
import time
import tty

FW_VERSION = "0.1.0"


class Harness:
    """Pure protocol logic — same behaviour as the .ino, host-testable."""

    def __init__(self) -> None:
        self.t0 = time.monotonic()
        self.led = False

    def _uptime_ms(self) -> int:
        return int((time.monotonic() - self.t0) * 1000)

    def handle(self, line: str) -> str | None:
        line = line.strip()
        if not line:
            return None  # firmware tolerates empty lines silently
        parts = line.split(" ", 1)
        cmd = parts[0].upper()
        rest = parts[1] if len(parts) > 1 else ""

        if cmd == "PING":
            return "OK PONG"
        if cmd == "VERSION":
            return f"OK {FW_VERSION}"
        if cmd == "UPTIME":
            return f"OK {self._uptime_ms()}"
        if cmd == "ECHO":
            return f"OK {rest}"
        if cmd == "LED":
            arg = rest.strip().upper()
            if arg == "ON":
                self.led = True
            elif arg == "OFF":
                self.led = False
            elif arg == "TOGGLE":
                self.led = not self.led
            else:
                return "ERR LED arg must be ON|OFF|TOGGLE"
            return f"OK {'ON' if self.led else 'OFF'}"
        if cmd == "ANALOG":
            return "OK 2048"  # midscale 12-bit reading
        if cmd == "DIGITAL":
            return "OK 0"
        if cmd == "SELFTEST":
            return "OK PASS"
        if cmd == "RESET":
            return "OK RESETTING"
        return f"ERR unknown: {cmd}"


def serve(port_file: str | None) -> int:
    master, slave = pty.openpty()
    # Raw mode: a real USB-serial link has no line discipline, so disable
    # echo, canonical buffering, and CR/NL translation on both ends.
    tty.setraw(master)
    tty.setraw(slave)
    slave_name = os.ttyname(slave)
    print(slave_name, flush=True)
    if port_file:
        with open(port_file, "w") as fh:
            fh.write(slave_name)

    harness = Harness()
    os.write(master, f"READY {FW_VERSION}\n".encode())

    buf = bytearray()
    while True:
        try:
            chunk = os.read(master, 1024)
        except OSError:
            break  # peer closed
        if not chunk:
            break
        for b in chunk:
            if b == 0x0D:  # '\r' ignored, like the firmware
                continue
            if b == 0x0A:  # '\n' -> dispatch
                resp = harness.handle(buf.decode("ascii", errors="replace"))
                buf.clear()
                if resp is not None:
                    os.write(master, (resp + "\n").encode())
            else:
                buf.append(b)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port-file", help="write the PTY slave path to this file")
    args = ap.parse_args()
    if not hasattr(pty, "openpty"):
        sys.stderr.write("PTY emulation requires a Unix host.\n")
        return 2
    try:
        return serve(args.port_file)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
