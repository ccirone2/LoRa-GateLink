#!/usr/bin/env bash
# The GateLink toolchain for a Claude Code cloud session (Linux x86_64): arduino-cli, the cores and libraries CI
# pins, the web console's npm tooling, and uv with the locked Python environment (pyproject.toml, uv.lock). The SessionStart hook (tools/agent/hooks.py) starts it in
# the background; it can also be a cloud environment's setup script. Idempotent: a step whose tool is already
# there is skipped. Writes ~/.cache/gatelink/setup.done (what's installed, and what failed) when it ends.
#
# Versions: keep in step with docs/development.md, .github/workflows/ci.yml and release.yml, and uv's with
# pyproject.toml's required-version
# (tests/tools/test_agent_tooling.py checks they agree).
set -uo pipefail

ARDUINO_CLI_VERSION=1.5.1
ARDUINO_CLI_SHA256=28a8e119c498a25607821c36cb2dc49e8463941b261a0d99091baa7bc692dd2b  # Linux_64bit, from its release
CORES=("arduino:samd@1.8.14" "arduino:avr@1.8.8")
LIBS=("LoRa@0.8.0" "Crypto@0.4.0" "FlashStorage@1.0.0" "ArduinoJson@7.4.3")
UV_VERSION=0.12.19
UV_SHA256=23bf5552d220e0842b65c862097b2ebaeba0064b74eda5e565e77fd25969d8c8  # x86_64-unknown-linux-gnu, from its release

root="$(cd "$(dirname "$0")/../.." && pwd)"
state="${XDG_CACHE_HOME:-$HOME/.cache}/gatelink"
bin="$HOME/.local/bin"
mkdir -p "$state" "$bin"
rm -f "$state/setup.done"
export PATH="$bin:$root/.venv/bin:$PATH"

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

uv_tool() {
  if uv --version 2>/dev/null | grep -q "^uv $UV_VERSION "; then return 0; fi
  local tmp
  tmp=$(mktemp -d)
  curl -fsSL -o "$tmp/uv.tgz"     "https://github.com/astral-sh/uv/releases/download/$UV_VERSION/uv-x86_64-unknown-linux-gnu.tar.gz" &&
    echo "$UV_SHA256  $tmp/uv.tgz" | sha256sum -c - &&
    tar -xzf "$tmp/uv.tgz" -C "$tmp" &&
    install -m 755 "$tmp/uv-x86_64-unknown-linux-gnu/uv" "$bin/uv"
}

# The locked environment in $root/.venv (ruff, pytest and the tools' dependencies); a no-op when it's current.
python_env() {
  (cd "$root" && uv sync --locked)
}

step arduino-cli arduino_cli
if [[ " ${ok[*]} " == *" arduino-cli "* ]]; then
  step cores cores
  step libraries libs
fi
step npm node_tools
step uv uv_tool
if [[ " ${ok[*]} " == *" uv "* ]]; then
  step python python_env
fi

{
  echo "ok: ${ok[*]:-none}"
  if [ ${#failed[@]} -gt 0 ]; then
    echo "failed: ${failed[*]} (log: $state/setup.log; the network may not reach downloads.arduino.cc, github.com or PyPI)"
  fi
} >"$state/setup.done"
cat "$state/setup.done"
