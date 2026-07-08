"""Message pack/unpack for the three wire messages, per protocol/PROTOCOL.md.

Payloads here exclude the CRC — framing.py owns that layer.
"""

import math
import struct
from dataclasses import dataclass

PROTO_VER = 1
DOF = 6
MAX_STEPS = 100
WATCHDOG_MS = 300
MAX_FIRST_STEP_RAD = 0.261799  # 15 degrees
STATE_HZ = 20

MSG_ACTION_CHUNK = 0x01
MSG_ENABLE = 0x02
MSG_STATE = 0x10

FLAG_ENABLED = 1 << 0
FLAG_FAULT_LATCHED = 1 << 1
FLAG_CHUNK_STARVED = 1 << 2
FLAG_CHUNK_REJECTED = 1 << 3
FLAG_ESTOP = 1 << 4

_CHUNK_HEADER = struct.Struct("<BHBBH")  # type, seq, dof, n, dt_ms
_STATE_HEADER = struct.Struct("<BBBBHIBBHH")


class MessageError(ValueError):
    pass


def seq_is_newer(seq: int, last: int) -> bool:
    """Serial-number arithmetic on u16: newer iff (seq-last) mod 2^16 in 1..0x7FFF."""
    return 0 < ((seq - last) & 0xFFFF) < 0x8000


@dataclass
class ActionChunk:
    seq: int
    dof: int
    dt_ms: int
    q: list[float]  # step-major, length n*dof

    @property
    def n(self) -> int:
        return len(self.q) // self.dof

    def pack(self) -> bytes:
        header = _CHUNK_HEADER.pack(
            MSG_ACTION_CHUNK, self.seq, self.dof, self.n, self.dt_ms
        )
        return header + struct.pack(f"<{len(self.q)}f", *self.q)

    @classmethod
    def unpack(cls, payload: bytes) -> "ActionChunk":
        if len(payload) < _CHUNK_HEADER.size:
            raise MessageError("chunk too short")
        msg_type, seq, dof, n, dt_ms = _CHUNK_HEADER.unpack_from(payload)
        if msg_type != MSG_ACTION_CHUNK:
            raise MessageError("not an ACTION_CHUNK")
        body = payload[_CHUNK_HEADER.size :]
        if dof == 0 or n == 0 or len(body) != 4 * n * dof:
            raise MessageError("chunk length inconsistent")
        q = list(struct.unpack(f"<{n * dof}f", body))
        return cls(seq=seq, dof=dof, dt_ms=dt_ms, q=q)


def chunk_admissible(
    chunk: ActionChunk, last_seq: int, commanded: list[float]
) -> tuple[bool, str]:
    """The admission rules from PROTOCOL.md; mirrored by firmware protocol.h."""
    if chunk.dof != DOF:
        return False, "dof mismatch"
    if not 1 <= chunk.n <= MAX_STEPS:
        return False, "bad step count"
    if any(not math.isfinite(v) for v in chunk.q):
        return False, "non-finite value"
    if not seq_is_newer(chunk.seq, last_seq):
        return False, "stale seq"
    for joint in range(DOF):
        if abs(chunk.q[joint] - commanded[joint]) > MAX_FIRST_STEP_RAD:
            return False, "first-step discontinuity"
    return True, ""


@dataclass
class Enable:
    enable: bool

    def pack(self) -> bytes:
        return bytes([MSG_ENABLE, 1 if self.enable else 0])

    @classmethod
    def unpack(cls, payload: bytes) -> "Enable":
        if len(payload) != 2 or payload[0] != MSG_ENABLE:
            raise MessageError("not an ENABLE")
        return cls(enable=payload[1] != 0)


@dataclass
class State:
    proto_ver: int
    fw_ver: int
    reset_cause: int
    seq_echo: int
    t_ms: int
    flags: int
    joint_valid: int
    crc_err_count: int
    clamp_count: int
    q_meas: list[float]

    def pack(self) -> bytes:
        header = _STATE_HEADER.pack(
            MSG_STATE,
            self.proto_ver,
            self.fw_ver,
            self.reset_cause,
            self.seq_echo,
            self.t_ms,
            self.flags,
            self.joint_valid,
            self.crc_err_count,
            self.clamp_count,
        )
        return header + struct.pack(f"<{len(self.q_meas)}f", *self.q_meas)

    @classmethod
    def unpack(cls, payload: bytes) -> "State":
        if len(payload) < _STATE_HEADER.size:
            raise MessageError("state too short")
        fields = _STATE_HEADER.unpack_from(payload)
        if fields[0] != MSG_STATE:
            raise MessageError("not a STATE")
        body = payload[_STATE_HEADER.size :]
        if len(body) % 4 != 0:
            raise MessageError("state length inconsistent")
        q_meas = list(struct.unpack(f"<{len(body) // 4}f", body))
        return cls(*fields[1:], q_meas=q_meas)


def unpack_any(payload: bytes) -> ActionChunk | Enable | State:
    if not payload:
        raise MessageError("empty payload")
    parsers = {
        MSG_ACTION_CHUNK: ActionChunk.unpack,
        MSG_ENABLE: Enable.unpack,
        MSG_STATE: State.unpack,
    }
    parser = parsers.get(payload[0])
    if parser is None:
        raise MessageError(f"unknown message type 0x{payload[0]:02x}")
    return parser(payload)
