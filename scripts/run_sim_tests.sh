#!/usr/bin/env bash
# Run the host-side HIL test suite against the software emulator (no board).
#
# Validates conftest.py, the serial command protocol, and the loop tests
# against scripts/giga_sim.py, which mirrors firmware/test_harness. Use this
# to exercise the pipeline anywhere a real GIGA is not attached (CI, laptops
# without the board). A green run here means everything except the physical
# hardware is good.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

PY="${PY:-.venv/bin/python}"
PYTEST="${PYTEST:-.venv/bin/pytest}"
[[ -x "$PY" ]] || PY="$(command -v python3)"
[[ -x "$PYTEST" ]] || PYTEST="$PY -m pytest"

PORT_FILE="$(mktemp)"
SIM_OUT="$(mktemp)"
SIM_PID=""

cleanup() {
  [[ -n "$SIM_PID" ]] && kill "$SIM_PID" 2>/dev/null || true
  rm -f "$PORT_FILE" "$SIM_OUT"
}
trap cleanup EXIT

echo "[sim] starting GIGA emulator"
"$PY" scripts/giga_sim.py --port-file "$PORT_FILE" >"$SIM_OUT" 2>&1 &
SIM_PID=$!

for _ in $(seq 1 50); do
  [[ -s "$PORT_FILE" ]] && break
  sleep 0.1
done
PORT="$(cat "$PORT_FILE" 2>/dev/null || true)"
if [[ -z "$PORT" ]]; then
  echo "[sim] emulator failed to start:" >&2
  cat "$SIM_OUT" >&2
  exit 1
fi
echo "[sim] emulator listening on $PORT"

# Short loop counts keep the host-only run fast; override via env if desired.
GIGA_PORT="$PORT" \
GIGA_PING_ITERS="${GIGA_PING_ITERS:-300}" \
GIGA_LED_ITERS="${GIGA_LED_ITERS:-100}" \
GIGA_SELFTEST_ITERS="${GIGA_SELFTEST_ITERS:-40}" \
  $PYTEST tests/ "$@"
