#!/usr/bin/env python3
"""Regenerate the golden wire-frame vectors from the host codec.

Output must be byte-identical to what is checked in; CI does not run this —
the .bin files are the truth both test suites verify against.
"""

import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "host"))
from giga_host.framing import encode_frame  # noqa: E402
from giga_host.messages import ActionChunk, Enable, State  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent


def q_ramp(count: int) -> list[float]:
    return [k / 1024 for k in range(count)]


def build_vectors() -> dict[str, bytes]:
    nominal = encode_frame(
        ActionChunk(seq=1, dof=6, dt_ms=33, q=q_ramp(50 * 6)).pack()
    )
    bad_crc = bytearray(nominal)
    bad_crc[-2] ^= 0xFF  # last byte before the 0x00 terminator is CRC high byte
    nan_q = q_ramp(50 * 6)
    nan_q[7] = float("nan")
    return {
        "chunk_nominal.bin": nominal,
        "chunk_teleop_n1.bin": encode_frame(
            ActionChunk(seq=2, dof=6, dt_ms=33, q=q_ramp(6)).pack()
        ),
        "chunk_bad_crc.bin": bytes(bad_crc),
        "chunk_nan.bin": encode_frame(
            ActionChunk(seq=1, dof=6, dt_ms=33, q=nan_q).pack()
        ),
        "chunk_dof_mismatch.bin": encode_frame(
            ActionChunk(seq=3, dof=5, dt_ms=33, q=q_ramp(2 * 5)).pack()
        ),
        "enable_on.bin": encode_frame(Enable(True).pack()),
        "enable_off.bin": encode_frame(Enable(False).pack()),
        "state_nominal.bin": encode_frame(
            State(
                proto_ver=1,
                fw_ver=1,
                reset_cause=0,
                seq_echo=1,
                t_ms=123456,
                flags=0x01,
                joint_valid=0x3F,
                crc_err_count=0,
                clamp_count=2,
                q_meas=[-j / 1024 for j in range(6)],
            ).pack()
        ),
    }


MANIFEST = {
    "chunk_nominal.bin": {"decodes": True, "type": "chunk", "admissible": True},
    "chunk_teleop_n1.bin": {"decodes": True, "type": "chunk", "admissible": True},
    "chunk_bad_crc.bin": {"decodes": False},
    "chunk_nan.bin": {
        "decodes": True,
        "type": "chunk",
        "admissible": False,
        "reject_reason": "non-finite value",
    },
    "chunk_dof_mismatch.bin": {
        "decodes": True,
        "type": "chunk",
        "admissible": False,
        "reject_reason": "dof mismatch",
    },
    "enable_on.bin": {"decodes": True, "type": "enable", "enable": True},
    "enable_off.bin": {"decodes": True, "type": "enable", "enable": False},
    "state_nominal.bin": {"decodes": True, "type": "state", "seq_echo": 1},
}

if __name__ == "__main__":
    for name, frame in build_vectors().items():
        (HERE / name).write_bytes(frame)
        print(f"{name}: {len(frame)} bytes")
    (HERE / "manifest.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
    # NaN encoding must be deterministic for a checked-in vector
    assert struct.pack("<f", float("nan")) == b"\x00\x00\xc0\x7f"
