"""Basic command/response tests over the GIGA R1 serial harness."""

from __future__ import annotations

import time


def test_ping(giga):
    assert giga.cmd("PING") == "OK PONG"


def test_version_present(giga):
    payload = giga.expect_ok("VERSION")
    assert payload, "VERSION returned empty payload"
    # Loose semver-ish check (major.minor.patch).
    parts = payload.split(".")
    assert len(parts) >= 2 and all(p.isdigit() for p in parts[:2]), payload


def test_echo_roundtrip(giga):
    payload = giga.expect_ok("ECHO hello world 123")
    assert payload == "hello world 123"


def test_uptime_monotonic(giga):
    a = int(giga.expect_ok("UPTIME"))
    time.sleep(0.05)
    b = int(giga.expect_ok("UPTIME"))
    assert b > a, f"uptime did not advance: {a} -> {b}"


def test_led_states(giga):
    assert giga.expect_ok("LED ON") == "ON"
    assert giga.expect_ok("LED OFF") == "OFF"
    assert giga.expect_ok("LED TOGGLE") == "ON"
    assert giga.expect_ok("LED TOGGLE") == "OFF"


def test_led_rejects_bad_arg(giga):
    resp = giga.cmd("LED MAYBE")
    assert resp.startswith("ERR "), resp


def test_unknown_command(giga):
    resp = giga.cmd("FLOOBLE")
    assert resp.startswith("ERR "), resp


def test_selftest(giga):
    assert giga.expect_ok("SELFTEST") == "PASS"
