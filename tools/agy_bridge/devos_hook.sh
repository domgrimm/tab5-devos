#!/bin/sh
# devOS Tab5 bridge: agy PreToolUse hook (installed by bridge_server.py).
# Outside bridge sessions it answers "ask", i.e. agy's normal permission flow.
if [ -z "$DEVOS_AGY_BRIDGE_SOCK" ]; then
    cat >/dev/null
    echo '{"decision":"ask"}'
    exit 0
fi
exec python3 "$(dirname "$0")/devos_hook.py"
