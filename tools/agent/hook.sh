#!/bin/sh
# Entry point for the Claude Code hooks in .claude/settings.json: runs tools/agent/hooks.py <event> with the first
# working Python 3 (python3 on Linux; on Windows, where python3 can be the Store stub, python). stdin passes through.
# No Python means no checks, never a broken session.
dir=$(dirname "$0")
for py in python3 python; do
  if "$py" -c 'import sys; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1; then
    exec "$py" "$dir/hooks.py" "$@"
  fi
done
echo "GateLink hooks: no Python 3 found, $1 checks skipped" >&2
exit 0
