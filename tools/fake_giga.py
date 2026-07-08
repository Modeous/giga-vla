#!/usr/bin/env python3
"""Giga firmware emulator over a pty, for hardware-free host testing.

Implements the protocol semantics from protocol/PROTOCOL.md: framing,
chunk admission, ENABLE/FAULT latching, the 300 ms watchdog, and 20 Hz
STATE emission with q_meas tracking the admitted trajectory. It does NOT
model servo dynamics — q_meas equals the interpolated commanded position.

CLI: prints the pty path to stdout, then serves until killed.
Tests: instantiate FakeGiga over any fd pair and call run() in a thread.
"""

import os
import pathlib
import pty
import select
import sys
import time
import tty

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "host"))
from giga_host import framing, messages  # noqa: E402

FW_VER = 1
TICK_S = 1 / 100
STATE_PERIOD_S = 1 / messages.STATE_HZ
WATCHDOG_S = messages.WATCHDOG_MS / 1000


class FakeGiga:
    def __init__(self, fd: int, dof: int = messages.DOF) -> None:
        self.fd = fd
        self.dof = dof
        self.parser = framing.FrameParser()
        self.enabled = False
        self.fault_latched = False
        self.estop = False
        self.chunk_rejected = False
        self.chunk_starved = False
        self.last_seq = 0xFFFF  # so seq=0 is "newer" on the first chunk
        self.clamp_count = 0
        self.commanded = [0.0] * dof
        self.chunk: messages.ActionChunk | None = None
        self.chunk_t0 = 0.0
        self.last_chunk_time: float | None = None
        self._stop = False

    def stop(self) -> None:
        self._stop = True

    def _handle(self, payload: bytes, now: float) -> None:
        try:
            msg = messages.unpack_any(payload)
        except messages.MessageError:
            # A CRC-valid but structurally inconsistent chunk is an
            # admission rejection (PROTOCOL.md rule 1), not silence.
            if payload and payload[0] == messages.MSG_ACTION_CHUNK:
                if self.enabled and not self.fault_latched:
                    self.chunk_rejected = True
            return
        if isinstance(msg, messages.Enable):
            if msg.enable and not self.estop:
                self.enabled = True
                self.fault_latched = False
                self.chunk_starved = False
                self.last_chunk_time = None
            else:
                self.enabled = False
                self.chunk = None
        elif isinstance(msg, messages.ActionChunk):
            if self.fault_latched or not self.enabled:
                return
            ok, _ = messages.chunk_admissible(msg, self.last_seq, self.commanded)
            self.chunk_rejected = not ok
            if ok:
                self.last_seq = msg.seq
                self.chunk = msg
                self.chunk_t0 = now
                self.last_chunk_time = now

    def _tick(self, now: float) -> None:
        if self.enabled and self.last_chunk_time is not None:
            if now - self.last_chunk_time > WATCHDOG_S:
                self.fault_latched = True
                self.chunk_starved = True
                self.enabled = False  # FAULT implies not enabled, like firmware
                self.chunk = None
        if self.chunk is not None:
            # linear interpolation along the chunk's dt grid
            step_f = (now - self.chunk_t0) / (self.chunk.dt_ms / 1000)
            last = self.chunk.n - 1
            i = min(int(step_f), last)
            frac = min(step_f - i, 1.0) if i < last else 0.0
            for j in range(self.dof):
                a = self.chunk.q[i * self.dof + j]
                b = self.chunk.q[min(i + 1, last) * self.dof + j]
                self.commanded[j] = a + (b - a) * frac

    def _state(self, now: float) -> messages.State:
        flags = (
            (messages.FLAG_ENABLED if self.enabled else 0)
            | (messages.FLAG_FAULT_LATCHED if self.fault_latched else 0)
            | (messages.FLAG_CHUNK_STARVED if self.chunk_starved else 0)
            | (messages.FLAG_CHUNK_REJECTED if self.chunk_rejected else 0)
            | (messages.FLAG_ESTOP if self.estop else 0)
        )
        return messages.State(
            proto_ver=messages.PROTO_VER,
            fw_ver=FW_VER,
            reset_cause=0,
            seq_echo=0 if self.last_seq == 0xFFFF else self.last_seq,
            t_ms=int(now * 1000) & 0xFFFFFFFF,
            flags=flags,
            joint_valid=(1 << self.dof) - 1,
            crc_err_count=self.parser.crc_err_count & 0xFFFF,
            clamp_count=self.clamp_count & 0xFFFF,
            q_meas=list(self.commanded),
        )

    def run(self) -> None:
        t0 = time.monotonic()
        next_state = t0
        while not self._stop:
            readable, _, _ = select.select([self.fd], [], [], TICK_S)
            now = time.monotonic() - t0
            if readable:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    return
                if not data:
                    return
                for payload in self.parser.feed(data):
                    self._handle(payload, now)
            self._tick(now)
            if time.monotonic() >= next_state:
                next_state += STATE_PERIOD_S
                try:
                    os.write(self.fd, framing.encode_frame(self._state(now).pack()))
                except OSError:
                    return


def main() -> None:
    manager_fd, worker_fd = pty.openpty()
    tty.setraw(worker_fd)  # binary protocol: no echo, no newline translation
    print(os.ttyname(worker_fd), flush=True)
    FakeGiga(manager_fd).run()


if __name__ == "__main__":
    main()
