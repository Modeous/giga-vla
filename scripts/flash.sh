#!/usr/bin/env bash
# Compile and flash a sketch to the attached GIGA R1.
# Usage: scripts/flash.sh [sketch_dir]
# Defaults to firmware/test_harness. Honours GIGA_PORT and FQBN env vars.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH_DIR="${1:-$REPO_ROOT/firmware/test_harness}"
FQBN="${FQBN:-arduino:mbed_giga:giga}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/$(basename "$SKETCH_DIR")}"

PORT="${GIGA_PORT:-$(python3 "$REPO_ROOT/scripts/detect_giga.py" --quiet)}"

echo "[flash] sketch=$SKETCH_DIR fqbn=$FQBN port=$PORT"
mkdir -p "$BUILD_DIR"
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" "$SKETCH_DIR"
arduino-cli upload  --fqbn "$FQBN" --input-dir  "$BUILD_DIR" -p "$PORT" "$SKETCH_DIR"
echo "[flash] done"
