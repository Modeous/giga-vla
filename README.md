# giga-vla

Hardware test pipeline for the Arduino GIGA R1.

Iterative firmware-flash + hardware-in-loop testing, driven locally on a Mac
and in CI via a self-hosted GitHub Actions runner physically wired to the
board.

## Quickstart (local, on the Mac with the GIGA attached)

```bash
./scripts/setup_mac.sh       # installs arduino-cli, mbed_giga core, .venv, deps
make detect                  # confirm the GIGA shows up
make iterate                 # compile + flash test_harness + run HIL tests
make watch                   # same, but auto-reruns on any firmware/tests change
```

## CI

`.github/workflows/hardware-ci.yml` runs on every push and PR:

- `build` (ubuntu-latest): compiles both `firmware/test_harness` and
  `firmware/main` via `arduino-cli`, uploads binaries as artifacts.
- `hil` (self-hosted macOS + `giga-r1` label): flashes the test harness to
  the connected GIGA R1, runs `pytest tests/`, uploads junit + HTML report.

A `concurrency: giga-r1-hardware` guard serialises HIL jobs so only one run
at a time owns the physical board.

To wire a Mac up as the HIL runner, follow [`runner/SETUP.md`](runner/SETUP.md).

## Layout

```
firmware/
  test_harness/   command-protocol sketch the pipeline flashes
  main/           application sketch skeleton
tests/            pytest HIL suite (pyserial)
scripts/          setup, port detection, flash, iterate
runner/           self-hosted GitHub Actions runner setup notes
docs/             pipeline architecture
.github/workflows/hardware-ci.yml
```

See [`docs/HARDWARE_PIPELINE.md`](docs/HARDWARE_PIPELINE.md) for the full
architecture and dev workflow.

## Test harness protocol

Newline-terminated commands over USB serial (115200 8N1). Each command
returns one line, prefixed `OK ` or `ERR `.

| Command            | Response             | Notes                    |
| ------------------ | -------------------- | ------------------------ |
| `PING`             | `OK PONG`            | health check             |
| `VERSION`          | `OK <semver>`        | firmware version         |
| `UPTIME`           | `OK <millis>`        | ms since boot            |
| `ECHO <text>`      | `OK <text>`          | round-trip               |
| `LED ON\|OFF\|TOGGLE` | `OK <state>`      | drives `LED_BUILTIN`     |
| `ANALOG <pin>`     | `OK <0..4095>`       | 12-bit ADC               |
| `DIGITAL <pin>`    | `OK <0\|1>`          | INPUT, then read         |
| `SELFTEST`         | `OK PASS` / `ERR ..` | clock + ADC sanity       |
| `RESET`            | `OK RESETTING`       | soft reset via NVIC      |
