"""Iterative load / soak tests for the GIGA R1.

Measures round-trip latency and verifies zero protocol loss across many
iterations. Tunable via env vars so CI can do a short pass and a nightly
job can do a long soak.
"""

from __future__ import annotations

import os
import statistics
import time


def _ival(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        return default


def test_ping_loop(giga):
    n = _ival("GIGA_PING_ITERS", 500)
    latencies_us: list[float] = []
    for i in range(n):
        t0 = time.perf_counter()
        resp = giga.cmd("PING", timeout=1.0)
        dt = (time.perf_counter() - t0) * 1e6
        assert resp == "OK PONG", f"iter {i}: got {resp!r}"
        latencies_us.append(dt)
    p50 = statistics.median(latencies_us)
    p99 = sorted(latencies_us)[int(0.99 * len(latencies_us)) - 1]
    print(f"\nPING loop: n={n} p50={p50:.0f}us p99={p99:.0f}us max={max(latencies_us):.0f}us")
    # Sanity guardrails — extremely loose; tighten once a baseline is known.
    assert p99 < 100_000, f"p99 latency too high: {p99:.0f}us"


def test_led_toggle_loop(giga):
    n = _ival("GIGA_LED_ITERS", 200)
    expected = "OFF"
    for i in range(n):
        expected = "ON" if expected == "OFF" else "OFF"
        got = giga.expect_ok("LED TOGGLE")
        assert got == expected, f"iter {i}: expected {expected}, got {got}"


def test_selftest_loop(giga):
    n = _ival("GIGA_SELFTEST_ITERS", 50)
    for i in range(n):
        assert giga.expect_ok("SELFTEST") == "PASS", f"selftest failed at iter {i}"
