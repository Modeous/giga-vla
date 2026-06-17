#!/usr/bin/env bash
# Install and start the GitHub Actions self-hosted runner for the GIGA HIL job.
#
# Usage:
#   scripts/install_runner.sh --url <repo_url> --token <runner_token> \
#                             [--name giga-mac] [--work _work] [--version 2.319.1]
#
# Get --url and --token from:
#   Settings → Actions → Runners → New self-hosted runner → macOS
# (Token is short-lived; copy it just before running this script.)
#
# Labels [self-hosted, macOS, giga-r1] match what hardware-ci.yml targets.

set -euo pipefail

URL=""
TOKEN=""
NAME="giga-mac"
WORK="_work"
VERSION="2.319.1"
RUNNER_DIR="${HOME}/actions-runner-giga"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)     URL="$2"; shift 2 ;;
    --token)   TOKEN="$2"; shift 2 ;;
    --name)    NAME="$2"; shift 2 ;;
    --work)    WORK="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --dir)     RUNNER_DIR="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$URL"   ]] || { echo "missing --url"   >&2; exit 2; }
[[ -n "$TOKEN" ]] || { echo "missing --token" >&2; exit 2; }

case "$(uname -m)" in
  arm64)  ARCH="arm64" ;;
  x86_64) ARCH="x64"   ;;
  *) echo "unsupported arch $(uname -m)" >&2; exit 2 ;;
esac
TARBALL="actions-runner-osx-${ARCH}-${VERSION}.tar.gz"
DL_URL="https://github.com/actions/runner/releases/download/v${VERSION}/${TARBALL}"

mkdir -p "$RUNNER_DIR"
cd "$RUNNER_DIR"

if [[ ! -x ./config.sh ]]; then
  echo "[runner] downloading $TARBALL"
  curl -fsSL -o "$TARBALL" "$DL_URL"
  echo "[runner] extracting"
  tar xzf "$TARBALL"
  rm -f "$TARBALL"
fi

if [[ -f .runner ]]; then
  echo "[runner] already configured at $RUNNER_DIR — skipping config"
else
  ./config.sh \
    --unattended \
    --url    "$URL" \
    --token  "$TOKEN" \
    --name   "$NAME" \
    --labels "giga-r1" \
    --work   "$WORK" \
    --replace
fi

# Install + start as launchd service so it survives reboots.
sudo ./svc.sh install   "$USER"
sudo ./svc.sh start
./svc.sh status || true

echo
echo "[runner] installed at: $RUNNER_DIR"
echo "[runner] verify in GitHub: Settings → Actions → Runners"
echo "[runner] next: trigger hardware-ci via 'gh workflow run hardware-ci.yml' or push a commit."
