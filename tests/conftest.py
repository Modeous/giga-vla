"""Pytest fixtures for GIGA R1 hardware-in-loop tests.

The `giga` fixture opens a serial connection to the attached board, drains the
boot banner, and exposes a small client with `cmd(line)` for synchronous
request/response and `send(line)` / `readline()` for finer-grained control.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
import serial

REPO_ROOT = Path(__file__).resolve().parent.parent
BAUD = 115200
DEFAULT_TIMEOUT = 2.0


def _detect_port() -> str:
    port = os.environ.get("GIGA_PORT")
    if port:
        return port
    result = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "detect_giga.py"), "--quiet"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        pytest.exit(f"GIGA not detected: {result.stderr.strip()}", returncode=2)
    return result.stdout.strip()


class GigaClient:
    def __init__(self, ser: serial.Serial):
        self.ser = ser

    def send(self, line: str) -> None:
        if not line.endswith("\n"):
            line += "\n"
        self.ser.write(line.encode("ascii"))
        self.ser.flush()

    def readline(self, timeout: float | None = None) -> str:
        if timeout is not None:
            self.ser.timeout = timeout
        raw = self.ser.readline()
        return raw.decode("ascii", errors="replace").rstrip("\r\n")

    def cmd(self, line: str, timeout: float = DEFAULT_TIMEOUT) -> str:
        self.ser.reset_input_buffer()
        self.send(line)
        return self.readline(timeout=timeout)

    def expect_ok(self, line: str, timeout: float = DEFAULT_TIMEOUT) -> str:
        resp = self.cmd(line, timeout=timeout)
        assert resp.startswith("OK "), f"expected OK, got: {resp!r}"
        return resp[3:]


@pytest.fixture(scope="session")
def giga_port() -> str:
    return _detect_port()


@pytest.fixture(scope="session")
def giga(giga_port: str):
    ser = serial.Serial(giga_port, BAUD, timeout=DEFAULT_TIMEOUT)
    # Drain any banner / leftover bytes from boot.
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not ser.read(ser.in_waiting or 1):
            break
    client = GigaClient(ser)
    # Sanity: confirm the harness answers PING before any test runs.
    resp = client.cmd("PING", timeout=2.0)
    if resp != "OK PONG":
        pytest.exit(
            f"GIGA test harness not responding (got {resp!r}). "
            "Did you flash firmware/test_harness?",
            returncode=2,
        )
    yield client
    ser.close()
