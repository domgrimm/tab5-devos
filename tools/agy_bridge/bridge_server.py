"""Antigravity bridge daemon (Path B): Tab5 <-> Antigravity runtime.

Listens on the Tailscale interface (default 100.77.11.92:8420) and speaks
newline-delimited JSON over a single WebSocket (`/ws`).

Protocol (both directions are JSON objects with a "type" field):

  Tab5 -> bridge:
    {"type":"HELLO","token":"<psk>","client":"devos/x.y"}
    {"type":"PROMPT","text":"...","command":"/goal"|""}
    {"type":"PERMISSION_REPLY","id":"...","allow":true,"always":false}
    {"type":"QUESTION_REPLY","id":"...","choice":1}
    {"type":"PING"}

  Bridge -> Tab5:
    {"type":"WELCOME","conversation_id","model","subagents":[{"name","state"}]}
    {"type":"THINKING","text"}            # appended to the thinking trace
    {"type":"TOKEN","text"}               # appended to the reply draft
    {"type":"TOOL","name","detail"}       # tool execution card
    {"type":"DIFF","file","hunk"}         # unified-diff chunk
    {"type":"ARTIFACT","name","kind","text"}
    {"type":"PERMISSION","id","text"}     # Tab5 must reply PERMISSION_REPLY
    {"type":"QUESTION","id","prompt","choices":[]}  # reply QUESTION_REPLY
    {"type":"SUBAGENTS","agents":[{"name","state"}]}
    {"type":"STATUS","state"}             # busy | idle
    {"type":"PONG"}
    {"type":"ERROR","message"}

Run:
  python3 bridge_server.py --host 100.77.11.92 --port 8420 --psk <secret>
  python3 bridge_server.py --demo   # scripted replies, no AGY install needed
"""

import argparse
import asyncio
import json
import time
import uuid
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from transcript_watcher import TranscriptWatcher, find_latest_transcript

app = FastAPI(title="devOS Antigravity Bridge Server")

PSK = ""
DEMO = False
CONVERSATION_ID = "41b1d485-b5bb-4ba8-b4a4-a1194ae1aa5c"
MODEL = "Gemini 3.8 Flash (High)"
watcher = TranscriptWatcher()


class PromptRequest(BaseModel):
    prompt: str
    slash_command: str = ""


@app.get("/status")
async def get_status():
    return {
        "status": "online",
        "active_transcript": find_latest_transcript(),
        "demo": DEMO,
        "service": "agy-bridge",
        "version": "2.0.0",
    }


def demo_script(prompt: str, command: str) -> List[Dict[str, Any]]:
    """Scripted turn used when no real Antigravity runtime is attached."""
    what = f"{command} " if command else ""
    return [
        {"type": "STATUS", "state": "busy"},
        {"type": "SUBAGENTS",
         "agents": [{"name": "research", "state": "running"},
                    {"name": "self", "state": "idle"}]},
        {"type": "THINKING",
         "text": f"Planning {what}request: {prompt[:120]}"},
        {"type": "THINKING", "text": "Checking workspace context."},
        {"type": "TOOL", "name": "view_file",
         "detail": "devos_config.h (128 lines)"},
        {"type": "TOKEN", "text": "Analysis complete. "},
        {"type": "TOKEN", "text": "Two files need attention. "},
        {"type": "PERMISSION", "id": f"perm-{uuid.uuid4().hex[:8]}",
         "text": "run_command 'ninja -C build_sim'"},
        {"type": "DIFF", "file": "main/apps/app_editor/app_editor.c",
         "hunk": "@@ -1,2 +1,3 @@\n+// demo hunk\n ctx();\n"},
        {"type": "ARTIFACT", "name": "plan.md", "kind": "markdown",
         "text": "# Demo plan\n\n- [x] Reproduce\n- [ ] Fix\n"},
        {"type": "QUESTION", "id": f"q-{uuid.uuid4().hex[:8]}",
         "prompt": "Which target should the fix land in?",
         "choices": ["factory", "ota_0", "both slots"]},
        {"type": "STATUS", "state": "idle"},
    ]


def transcript_to_events() -> List[Dict[str, Any]]:
    """Forward new transcript lines: text-ish lines become tokens."""
    out = []
    for ev in watcher.get_events():
        if not isinstance(ev, dict):
            continue
        text = ev.get("text") or ev.get("content") or ev.get("message")
        if isinstance(text, str) and text.strip():
            out.append({"type": "TOKEN", "text": text[:500]})
    return out


async def send(ws: WebSocket, msg: Dict[str, Any]):
    await ws.send_text(json.dumps(msg) + "\n")


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    authed = PSK == ""
    if authed:
        await send(websocket, {
            "type": "WELCOME",
            "conversation_id": CONVERSATION_ID,
            "model": MODEL,
            "subagents": [{"name": "research", "state": "idle"},
                          {"name": "self", "state": "idle"}],
        })
    pending_demo: List[Dict[str, Any]] = []

    try:
        while True:
            try:
                raw = await asyncio.wait_for(websocket.receive_text(),
                                             timeout=0.2)
            except asyncio.TimeoutError:
                raw = None
            if raw:
                try:
                    msg = json.loads(raw)
                except (ValueError, TypeError):
                    await send(websocket, {"type": "ERROR",
                                           "message": "bad json"})
                    continue
                mtype = msg.get("type", "")
                if mtype == "HELLO":
                    if PSK and msg.get("token") != PSK:
                        await send(websocket, {"type": "ERROR",
                                               "message": "bad token"})
                        break
                    authed = True
                    await send(websocket, {
                        "type": "WELCOME",
                        "conversation_id": CONVERSATION_ID,
                        "model": MODEL,
                        "subagents": [{"name": "research", "state": "idle"},
                                      {"name": "self", "state": "idle"}],
                    })
                elif not authed:
                    await send(websocket, {"type": "ERROR",
                                           "message": "hello first"})
                elif mtype == "PING":
                    await send(websocket, {"type": "PONG"})
                elif mtype == "PROMPT":
                    text = str(msg.get("text", ""))[:2000]
                    command = str(msg.get("command", ""))[:32]
                    print(f"[AGY-BRIDGE] prompt ({command or 'chat'}): "
                          f"{text[:80]}", flush=True)
                    if DEMO:
                        pending_demo.extend(demo_script(text, command))
                    else:
                        # Real runtime hook: transcript tail answers.
                        pending_demo.append({"type": "STATUS",
                                             "state": "busy"})
                elif mtype == "PERMISSION_REPLY":
                    print(f"[AGY-BRIDGE] permission {msg.get('id')}: "
                          f"allow={msg.get('allow')} "
                          f"always={msg.get('always')}", flush=True)
                    await send(websocket, {"type": "STATUS", "state": "idle"})
                elif mtype == "QUESTION_REPLY":
                    print(f"[AGY-BRIDGE] question {msg.get('id')}: "
                          f"choice={msg.get('choice')}", flush=True)
                    await send(websocket, {"type": "STATUS", "state": "idle"})
                else:
                    await send(websocket, {"type": "ERROR",
                                           "message": f"unknown {mtype}"})
            # flush one queued event per tick (pacing for the Tab5)
            if authed:
                if pending_demo:
                    await send(websocket, pending_demo.pop(0))
                    await asyncio.sleep(0.4)
                elif not DEMO:
                    for ev in transcript_to_events():
                        await send(websocket, ev)
    except WebSocketDisconnect:
        print("[AGY-BRIDGE] Tab5 client disconnected.")
    except Exception as e:
        print(f"[AGY-BRIDGE] Error: {e}")


if __name__ == "__main__":
    import uvicorn
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0", help="Host interface")
    parser.add_argument("--port", type=int, default=8420, help="Port")
    parser.add_argument("--psk", default="", help="Pre-shared token")
    parser.add_argument("--demo", action="store_true",
                        help="Scripted replies, no AGY runtime needed")
    args = parser.parse_args()
    PSK = args.psk
    DEMO = args.demo
    print(f"Starting agy-bridge on {args.host}:{args.port} "
          f"(demo={DEMO}, auth={'on' if PSK else 'off'})...")
    uvicorn.run(app, host=args.host, port=args.port)
