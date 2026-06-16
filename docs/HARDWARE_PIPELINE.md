# Hardware test pipeline

End-to-end CI/CD for iterative firmware development on the Arduino GIGA R1.

```
┌──────────────┐   git push    ┌──────────────────┐   dispatch    ┌────────────────────┐
│  Developer   │ ────────────▶ │  GitHub Actions  │ ────────────▶ │  Self-hosted Mac   │
│  laptop      │               │  hardware-ci.yml │               │  + GIGA R1 via USB │
└──────────────┘               └──────────────────┘               └─────────┬──────────┘
                                       │                                    │
                                       │ build (hosted)                     │ hil (self-hosted)
                                       ▼                                    ▼
                              arduino-cli compile              arduino-cli compile + upload
                              firmware artifact                pytest HIL suite
                                                               junit + html report
```

## Components

- **Firmware**
  - `firmware/test_harness/` — sketch the pipeline flashes. Implements a
    newline-delimited serial command protocol (`PING`, `VERSION`, `LED`,
    `ECHO`, `ANALOG`, `DIGITAL`, `SELFTEST`, `UPTIME`, `RESET`).
  - `firmware/main/` — application skeleton. Compiled in CI so production
    builds break early.
- **Host tooling**
  - `scripts/setup_mac.sh` — idempotent toolchain bootstrap.
  - `scripts/detect_giga.py` — find the GIGA's serial port by VID/PID/name,
    overridable via `GIGA_PORT`.
  - `scripts/flash.sh` — compile + upload one sketch.
  - `scripts/iterate.sh` — flash + test, with `--watch` for fswatch loops.
- **Tests** (`tests/`)
  - `conftest.py` — `giga` fixture: opens the serial port, drains the boot
    banner, sanity-pings the harness, exposes `cmd` / `expect_ok` helpers.
  - `test_basic.py` — protocol correctness.
  - `test_loop.py` — soak/iteration: PING latency p50/p99, LED toggle
    consistency, repeated SELFTEST. Iteration counts tunable via env vars.
- **CI** (`.github/workflows/hardware-ci.yml`)
  - `build` job on `ubuntu-latest` — pure compile gate for every PR.
  - `hil` job on `[self-hosted, macOS, giga-r1]` — flashes and tests on real
    hardware. Guarded by `concurrency: giga-r1-hardware` so only one run at
    a time touches the board.

## Iterative dev loop on the Mac

```bash
make setup                   # one time
make watch                   # auto flash + test on every save
# or, single shot:
make iterate
```

`scripts/iterate.sh --watch` uses `fswatch` to recompile, reflash, and re-run
the HIL suite whenever anything in `firmware/` or `tests/` changes. Failures
print to the terminal; watch mode continues so you can fix and retry.

## CI iteration loop

Every push and PR runs `hardware-ci`:

1. Ubuntu `build` job compiles both sketches and uploads the binaries.
2. macOS self-hosted `hil` job flashes `firmware/test_harness` to the real
   GIGA and runs the pytest HIL suite end-to-end.
3. Reports (`report.xml`, `report.html`) are attached as artifacts on every
   run, pass or fail.

Tune the hardware loop length per-run from the **Run workflow** UI
(`ping_iters`, `led_iters` inputs) or via env vars locally
(`GIGA_PING_ITERS`, `GIGA_LED_ITERS`, `GIGA_SELFTEST_ITERS`).

## Adding a new test

1. Add a command handler in `firmware/test_harness/test_harness.ino` and
   bump `FW_VERSION`.
2. Add a pytest in `tests/test_basic.py` (or a new file) that drives the
   command via the `giga` fixture.
3. `make iterate` locally to confirm the loop is green.
4. Push — CI flashes the new harness to the real board and runs the suite.

## Adding a new firmware target

Drop another sketch under `firmware/<name>/<name>.ino`, add it to the
`build` job in `.github/workflows/hardware-ci.yml`, and (optionally) point
`SKETCH=firmware/<name> make flash` for ad-hoc flashing.
