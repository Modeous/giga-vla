import json
import math
import pathlib

import pytest

from giga_host.framing import FrameParser, encode_frame
from giga_host.messages import (
    DOF,
    ActionChunk,
    Enable,
    State,
    chunk_admissible,
    seq_is_newer,
    unpack_any,
)

VECTORS = pathlib.Path(__file__).resolve().parents[2] / "protocol" / "vectors"
MANIFEST = json.loads((VECTORS / "manifest.json").read_text())


@pytest.mark.parametrize("name", sorted(MANIFEST))
def test_golden_vector(name):
    expected = MANIFEST[name]
    parser = FrameParser()
    payloads = parser.feed((VECTORS / name).read_bytes())

    if not expected["decodes"]:
        assert payloads == []
        assert parser.crc_err_count == 1
        return

    assert len(payloads) == 1 and parser.crc_err_count == 0
    msg = unpack_any(payloads[0])

    if expected["type"] == "chunk":
        assert isinstance(msg, ActionChunk)
        ok, reason = chunk_admissible(msg, last_seq=0, commanded=[0.0] * DOF)
        assert ok == expected["admissible"]
        if not ok:
            assert reason == expected["reject_reason"]
        # re-encoding reproduces the checked-in bytes exactly
        assert encode_frame(msg.pack()) == (VECTORS / name).read_bytes()
    elif expected["type"] == "enable":
        assert isinstance(msg, Enable) and msg.enable == expected["enable"]
    elif expected["type"] == "state":
        assert isinstance(msg, State) and msg.seq_echo == expected["seq_echo"]
        assert len(msg.q_meas) == DOF


def test_state_roundtrip():
    state = State(
        proto_ver=1,
        fw_ver=1,
        reset_cause=1,
        seq_echo=42,
        t_ms=99999,
        flags=0x0A,
        joint_valid=0x3F,
        crc_err_count=7,
        clamp_count=3,
        q_meas=[0.5, -0.5, 0.25, 0.0, 1.0, -1.0],
    )
    assert State.unpack(state.pack()) == state


def test_seq_is_newer_wraps():
    assert seq_is_newer(1, 0)
    assert not seq_is_newer(0, 0)
    assert not seq_is_newer(0, 1)
    assert seq_is_newer(0, 0xFFFF)  # wrap
    assert not seq_is_newer(0xFFFF, 0)
    assert seq_is_newer(0x7FFF, 0)
    assert not seq_is_newer(0x8000, 0)


def test_admission_stale_seq_and_discontinuity():
    chunk = ActionChunk(seq=5, dof=DOF, dt_ms=33, q=[0.0] * DOF)
    assert chunk_admissible(chunk, last_seq=5, commanded=[0.0] * DOF) == (
        False,
        "stale seq",
    )
    ok, _ = chunk_admissible(chunk, last_seq=4, commanded=[0.0] * DOF)
    assert ok
    teleport = ActionChunk(seq=6, dof=DOF, dt_ms=33, q=[math.pi / 2] + [0.0] * (DOF - 1))
    assert chunk_admissible(teleport, last_seq=5, commanded=[0.0] * DOF) == (
        False,
        "first-step discontinuity",
    )
