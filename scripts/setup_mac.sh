#!/usr/bin/env bash
# Idempotent setup for running the GIGA R1 hardware test pipeline on macOS.
# Installs arduino-cli, the mbed_giga core, and the Python test deps.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

log() { printf '[setup] %s\n' "$*"; }

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required. Install from https://brew.sh and re-run." >&2
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  log "installing arduino-cli"
  brew install arduino-cli
else
  log "arduino-cli present: $(arduino-cli version | head -1)"
fi

if ! command -v python3 >/dev/null 2>&1; then
  log "installing python"
  brew install python
fi

log "updating arduino core index"
arduino-cli core update-index

if ! arduino-cli core list 2>/dev/null | grep -q '^arduino:mbed_giga'; then
  log "installing arduino:mbed_giga core"
  arduino-cli core install arduino:mbed_giga
else
  log "arduino:mbed_giga already installed"
fi

if [[ ! -d "$REPO_ROOT/.venv" ]]; then
  log "creating .venv"
  python3 -m venv "$REPO_ROOT/.venv"
fi

# shellcheck disable=SC1091
source "$REPO_ROOT/.venv/bin/activate"
pip install --upgrade pip >/dev/null
pip install -r "$REPO_ROOT/tests/requirements.txt"

log "ok. activate with: source .venv/bin/activate"
