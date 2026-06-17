#!/usr/bin/env bash
# Iterative dev loop: flash firmware then run the HIL test suite, repeatedly.
# - Single shot: scripts/iterate.sh
# - Watch mode: scripts/iterate.sh --watch  (requires fswatch)
#
# Exits non-zero if any iteration fails so it composes with CI.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

WATCH=0
[[ "${1:-}" == "--watch" ]] && WATCH=1

run_once() {
  local rc=0
  ./scripts/flash.sh firmware/test_harness || rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "[iterate] flash failed (rc=$rc)" >&2
    return $rc
  fi
  # Give the board a moment to re-enumerate after upload.
  sleep 2
  GIGA_PORT="${GIGA_PORT:-$(python3 scripts/detect_giga.py --quiet)}" \
    .venv/bin/pytest tests/ -v --maxfail=1
}

if [[ $WATCH -eq 0 ]]; then
  run_once
  exit $?
fi

if ! command -v fswatch >/dev/null 2>&1; then
  echo "fswatch required for --watch. brew install fswatch" >&2
  exit 1
fi

echo "[iterate] watching firmware/ and tests/ — Ctrl-C to stop"
run_once || true
fswatch -o firmware tests | while read -r _; do
  echo "[iterate] change detected, re-running"
  run_once || echo "[iterate] iteration failed, continuing watch"
done
