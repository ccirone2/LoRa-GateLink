#!/usr/bin/env bash
# The GateLink toolchain for a Claude Code cloud session (Linux x86_64): arduino-cli, the cores and libraries CI
# pins, the web console's npm tooling, ruff and pytest. The SessionStart hook (tools/agent/hooks.py) starts it in
# the background; it can also be a cloud environment's setup script. Idempotent: a step whose tool is already
# there is skipped. Writes ~/.cache/gatelink/setup.done (what's installed, and what failed) when it ends.
#
# Versions: keep in step with docs/development.md, .github/workflows/ci.yml and release.yml
# (tests/tools/test_agent_tooling.py checks they agree).
set -uo pipefail

ARDUINO_CLI_VERSION=1.5.1
ARDUINO_CLI_SHA256=28a8e119c498a25607821c36cb2dc49e8463941b261a0d99091baa7bc692dd2b  # Linux_64bit, from its release
CORES=("arduino:samd@1.8.14" "arduino:avr@1.8.8")
LIBS=("LoRa@0.8.0" "Crypto@0.4.0" "FlashStorage@1.0.0" "ArduinoJson@7.4.3" "Adafruit SleepyDog Library@1.8.4")
RUFF_VERSION=0.16.10

root="$(cd "$(dirname "$0")/../.." && pwd)"
state="${XDG_CACHE_HOME:-$HOME/.cache}/gatelink"
bin="$HOME/.local/bin"
venv="$state/venv"
mkdir -p "$state" "$bin"
rm -f "$state/setup.done"
export PATH="$bin:$venv/bin:$PATH"

ok=()
failed=()
step() {
  local name=$1
  shift
  echo "== $name ($(date -u +%H:%M:%S))"
  if "$@"; then ok+=("$name"); else echo "!! $name failed"; failed+=("$name"); fi
}

arduino_cli() {
  if arduino-cli version 2>/dev/null | grep -q "Version: $ARDUINO_CLI_VERSION"; then return 0; fi
  local tmp
  tmp=$(mktemp -d)
  curl -fsSL -o "$tmp/cli.tgz" \
    "https://github.com/arduino/arduino-cli/releases/download/v$ARDUINO_CLI_VERSION/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" &&
    echo "$ARDUINO_CLI_SHA256  $tmp/cli.tgz" | sha256sum -c - &&
    tar -xzf "$tmp/cli.tgz" -C "$tmp" arduino-cli &&
    install -m 755 "$tmp/arduino-cli" "$bin/arduino-cli"
}

cores() {
  arduino-cli core update-index && arduino-cli core install "${CORES[@]}"
}

libs() {
  arduino-cli lib install "${LIBS[@]}"
}

node_tools() {
  [ -d "$root/node_modules/eslint" ] && return 0
  (cd "$root" && npm ci --no-audit --no-fund)
}

python_tools() {
  [ -x "$venv/bin/ruff" ] && [ -x "$venv/bin/pytest" ] && return 0
  python3 -m venv "$venv" && "$venv/bin/pip" install -q "ruff==$RUFF_VERSION" -r "$root/tests/e2e/requirements.txt"
}

step arduino-cli arduino_cli
if [[ " ${ok[*]} " == *" arduino-cli "* ]]; then
  step cores cores
  step libraries libs
fi
step npm node_tools
step python python_tools

{
  echo "ok: ${ok[*]:-none}"
  if [ ${#failed[@]} -gt 0 ]; then
    echo "failed: ${failed[*]} (log: $state/setup.log; the network may not reach downloads.arduino.cc or github.com)"
  fi
} >"$state/setup.done"
cat "$state/setup.done"
