# Self-hosted GitHub Actions runner (MacBook Pro + GIGA R1)

The `hardware-ci` workflow runs hardware-in-loop tests against a real Arduino
GIGA R1 connected over USB. That requires a self-hosted GitHub Actions runner
on the Mac that physically owns the board.

## 1. Prerequisites

- macOS 13+ on the MacBook Pro
- Homebrew installed (https://brew.sh)
- A GIGA R1 plugged into the Mac via USB

Install the local toolchain once:

```bash
./scripts/setup_mac.sh
```

This installs `arduino-cli`, the `arduino:mbed_giga` core, and a `.venv` with
the Python test deps.

## 2. Register the runner

You have two options. The scripted path is faster; the manual path is what
GitHub documents.

### Option A — scripted (recommended)

1. In GitHub, open the repo → **Settings** → **Actions** → **Runners** →
   **New self-hosted runner** → **macOS**. Note the **repo URL** and the
   short-lived **registration token** shown on that page.
2. From the repo root on the Mac:

   ```bash
   ./scripts/install_runner.sh \
     --url   https://github.com/Modeous/giga-vla \
     --token <REGISTRATION_TOKEN>
   ```

   This downloads the runner, configures it under `~/actions-runner-giga` with
   the `giga-r1` label, installs it as a launchd service, and starts it. The
   default name is `giga-mac`; override with `--name`.

### Option B — manual

1. On the same **New self-hosted runner** page, follow the download/configure
   commands shown. When prompted for labels, add `giga-r1` in addition to the
   defaults. Use a dedicated work dir, e.g. `~/actions-runner-giga`.

## 3. Run as a launchd service

So the runner survives reboots and reconnects:

```bash
cd ~/actions-runner
./svc.sh install
./svc.sh start
./svc.sh status
```

Tail logs:

```bash
tail -f ~/Library/Logs/actions.runner.*/Runner_*.log
```

## 4. Permissions

The runner user needs USB access to the GIGA. On macOS this is usually
automatic, but if `arduino-cli upload` reports a permission error:

- Grant **Full Disk Access** (and, on some versions, **Developer Tools**) to
  `/usr/local/opt/runner/.../actions-runner` in **System Settings → Privacy &
  Security**.
- Confirm the device appears: `ls /dev/cu.usbmodem*`.

## 5. Verify the runner sees the board

From the runner's shell:

```bash
python3 scripts/detect_giga.py --list
python3 scripts/detect_giga.py --quiet   # prints the device path or exits 2
```

## 6. Trigger a run

Push a commit, open a PR, or manually dispatch the `hardware-ci` workflow.
The `hil` job will:

1. Detect the GIGA serial port.
2. Compile + flash `firmware/test_harness`.
3. Run the pytest HIL suite.
4. Upload `report.xml` / `report.html` as workflow artifacts.

A `concurrency: giga-r1-hardware` guard ensures only one job at a time owns
the physical board.

## 7. Troubleshooting

- **`No GIGA-like serial device found`** — replug the board, re-run
  `detect_giga.py --list`, or set `GIGA_PORT=/dev/cu.usbmodemXXXX` explicitly
  in the runner's environment.
- **DFU stuck after a bad firmware** — double-tap the GIGA reset button to
  re-enter the bootloader, then re-run `make flash`.
- **Runner offline** — `./svc.sh status`; restart with `./svc.sh stop && ./svc.sh start`.
