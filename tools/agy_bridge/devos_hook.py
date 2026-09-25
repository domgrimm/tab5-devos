#!/usr/bin/env python3
"""agy PreToolUse hook for bridge sessions: ask the devOS bridge (Unix socket
in $DEVOS_AGY_BRIDGE_SOCK) whether this tool call may run. Bridge sessions run
agy with --dangerously-skip-permissions, so anything but a clear "allow" from
the bridge must deny (fail closed)."""
import json
import os
import socket
import sys

DENY = {"decision": "deny", "reason": "The devOS bridge is not reachable, so this tool call was blocked."}


def main():
    raw = sys.stdin.read()
    try:
        payload = json.loads(raw or "{}")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(None)                       # waiting for a human is fine
        s.connect(os.environ["DEVOS_AGY_BRIDGE_SOCK"])
        s.sendall((json.dumps(payload) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        answer = json.loads(buf.decode() or "{}")
        if answer.get("decision") not in ("allow", "deny", "ask", "force_ask"):
            answer = DENY
    except Exception:
        answer = DENY
    print(json.dumps(answer))


if __name__ == "__main__":
    main()
