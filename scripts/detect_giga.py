#!/usr/bin/env python3
"""Detect the serial port for an attached Arduino GIGA R1.

Honours the GIGA_PORT environment variable for explicit override. Otherwise
scans pyserial's list_ports for an Arduino-vendor device whose product or
description identifies it as a GIGA. Prints the device path and exits 0 on
success, or exits 2 with a diagnostic on failure.
"""

from __future__ import annotations

import argparse
import os
import sys

ARDUINO_VID = 0x2341
GIGA_PIDS = {0x0266, 0x0366, 0x0466, 0x0566}  # known GIGA R1 / DFU PIDs
NAME_HINTS = ("giga", "portenta")  # GIGA shares the mbed_giga core with Portenta


def find_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.stderr.write(
            "pyserial not installed. Run: pip install -r tests/requirements.txt\n"
        )
        sys.exit(2)
    return list(list_ports.comports())


def score(port) -> int:
    s = 0
    if port.vid == ARDUINO_VID:
        s += 10
    if port.pid in GIGA_PIDS:
        s += 20
    blob = " ".join(
        str(x or "") for x in (port.product, port.description, port.manufacturer)
    ).lower()
    if any(h in blob for h in NAME_HINTS):
        s += 5
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="print only the device path")
    ap.add_argument("--list", action="store_true", help="list all serial ports")
    args = ap.parse_args()

    override = os.environ.get("GIGA_PORT")
    if override:
        print(override)
        return 0

    ports = find_ports()
    if args.list:
        for p in ports:
            print(
                f"{p.device}\tvid={p.vid:#06x}\tpid={p.pid:#06x}\t"
                f"{p.manufacturer}\t{p.product}\t{p.description}"
                if p.vid is not None
                else f"{p.device}\t(no usb info)\t{p.description}"
            )
        return 0

    ranked = sorted(((score(p), p) for p in ports), key=lambda x: -x[0])
    if not ranked or ranked[0][0] == 0:
        sys.stderr.write(
            "No GIGA-like serial device found. Set GIGA_PORT to override, "
            "or run with --list to inspect attached ports.\n"
        )
        return 2

    best_score, best = ranked[0]
    if not args.quiet:
        sys.stderr.write(
            f"detected {best.device} score={best_score} "
            f"vid={best.vid:#06x} pid={best.pid:#06x} product={best.product}\n"
        )
    print(best.device)
    return 0


if __name__ == "__main__":
    sys.exit(main())
