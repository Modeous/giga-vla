"""Host <-> fake_giga soak over a pty: 10 Hz chunks, 20 Hz STATE, zero errors.

Duration defaults to 10 s so CI stays fast; the Milestone 1 acceptance run is
SOAK_SECONDS=600 (see docs/ARCHITECTURE.md).
"""

import math
import os
import pathlib
import pty
import sys
import threading
import time
import tty

from giga_host.link import Link
from giga_host.messages import (
    DOF,
    FLAG_CHUNK_REJECTED,
    FLAG_ENABLED,
    FLAG_FAULT_LATCHED,
    ActionChunk,
)

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from fake_giga import FakeGiga  # noqa: E402

SOAK_SECONDS = float(os.environ.get("SOAK_SECONDS", "10"))
CHUNK_HZ = 10
CHUNK_N = 50
DT_MS = 33


class PtyTransport:
    def __init__(self, fd: int) -> None:
        self.fd = fd
        os.set_blocking(fd, False)

    def read(self, n: int) -> bytes:
        try:
            return os.read(self.fd, n)
        except BlockingIOError:
            return b""

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)


def trajectory_chunk(seq: int, t0: float) -> ActionChunk:
    """A slow sine on every joint, continuous across chunks (small steps)."""
    q = []
    for step in range(CHUNK_N):
        t = t0 + step * DT_MS / 1000
        q += [0.2 * math.sin(0.5 * t + j) for j in range(DOF)]
    return ActionChunk(seq=seq, dof=DOF, dt_ms=DT_MS, q=q)


def test_soak():
    host_fd, giga_fd = pty.openpty()
    tty.setraw(giga_fd)  # binary protocol: no echo, no newline translation
    giga = FakeGiga(host_fd)  # emulator owns the manager end
    thread = threading.Thread(target=giga.run, daemon=True)
    thread.start()

    link = Link(PtyTransport(giga_fd))
    states = []
    try:
        link.send_enable(True)
        start = time.monotonic()
        seq = 0
        next_chunk = start
        while time.monotonic() - start < SOAK_SECONDS:
            now = time.monotonic()
            if now >= next_chunk:
                seq = (seq + 1) & 0xFFFF
                link.send_chunk(trajectory_chunk(seq, now - start))
                next_chunk += 1 / CHUNK_HZ
            states += link.poll()
            time.sleep(0.005)
        final_seq = seq
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:  # drain trailing STATEs
            states += link.poll()
            time.sleep(0.005)
    finally:
        giga.stop()
        thread.join(timeout=2)
        os.close(giga_fd)
        os.close(host_fd)

    # Both directions clean: no CRC errors seen by either side.
    assert link.crc_err_count == 0
    assert all(s.crc_err_count == 0 for s in states)
    # STATE arrived at ~20 Hz throughout.
    assert len(states) >= SOAK_SECONDS * 15
    # Never faulted, never rejected a chunk, stayed enabled.
    assert all(s.flags & FLAG_FAULT_LATCHED == 0 for s in states)
    assert all(s.flags & FLAG_CHUNK_REJECTED == 0 for s in states)
    assert states[-1].flags & FLAG_ENABLED
    # seq_echo caught up with what we sent.
    assert states[-1].seq_echo == final_seq
    # q_meas tracked the commanded sine (loose bound: interpolation + timing).
    expected = [0.2 * math.sin(0.5 * SOAK_SECONDS + j) for j in range(DOF)]
    for measured, target in zip(states[-1].q_meas, expected):
        assert abs(measured - target) < 0.1


def test_watchdog_latches_on_starvation():
    host_fd, giga_fd = pty.openpty()
    tty.setraw(giga_fd)
    giga = FakeGiga(host_fd)
    thread = threading.Thread(target=giga.run, daemon=True)
    thread.start()

    link = Link(PtyTransport(giga_fd))
    try:
        link.send_enable(True)
        link.send_chunk(ActionChunk(seq=1, dof=DOF, dt_ms=DT_MS, q=[0.0] * DOF))
        time.sleep(0.6)  # > WATCHDOG_MS with margin
        states = link.poll()
        assert states, "no STATE received"
        assert states[-1].flags & FLAG_FAULT_LATCHED
        # a fresh chunk must NOT clear the fault (no auto-resume)
        link.send_chunk(ActionChunk(seq=2, dof=DOF, dt_ms=DT_MS, q=[0.0] * DOF))
        time.sleep(0.1)
        states = link.poll()
        assert states[-1].flags & FLAG_FAULT_LATCHED
        assert states[-1].seq_echo == 1
        # explicit re-ENABLE clears it
        link.send_enable(True)
        link.send_chunk(ActionChunk(seq=3, dof=DOF, dt_ms=DT_MS, q=[0.0] * DOF))
        time.sleep(0.1)
        states = link.poll()
        assert states[-1].flags & FLAG_FAULT_LATCHED == 0
        assert states[-1].seq_echo == 3
    finally:
        giga.stop()
        thread.join(timeout=2)
        os.close(giga_fd)
        os.close(host_fd)
