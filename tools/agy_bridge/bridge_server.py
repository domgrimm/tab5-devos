"""
FastAPI & WebSocket bridge server for Antigravity Path B native client.
Listens on Tailscale interface (100.77.11.92:8420).
"""

import asyncio
import json
import argparse
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic import BaseModel
from transcript_watcher import TranscriptWatcher, find_latest_transcript

app = FastAPI(title="devOS Antigravity Bridge Server")
watcher = TranscriptWatcher()

class PromptRequest(BaseModel):
    prompt: str
    slash_command: str = ""

@app.get("/status")
async def get_status():
    transcript_path = find_latest_transcript()
    return {
        "status": "online",
        "active_transcript": transcript_path,
        "service": "agy-bridge",
        "version": "1.0.0"
    }

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    print("[AGY-BRIDGE] Tab5 client connected.")

    # Send initial greeting and conversation metadata
    await websocket.send_json({
        "type": "HANDSHAKE_ACK",
        "conversation_id": "41b1d485-b5bb-4ba8-b8a4-a1194ae1aa5c",
        "model": "Gemini 3.8 Flash (High)",
        "subagents": ["research", "self"]
    })

    try:
        while True:
            # Poll for new transcript events
            events = list(watcher.get_events())
            for event in events:
                await websocket.send_json({
                    "type": "TRANSCRIPT_EVENT",
                    "payload": event
                })

            await asyncio.sleep(0.5)
    except WebSocketDisconnect:
        print("[AGY-BRIDGE] Tab5 client disconnected.")
    except Exception as e:
        print(f"[AGY-BRIDGE] Error: {e}")

if __name__ == "__main__":
    import uvicorn
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0", help="Host interface")
    parser.add_argument("--port", type=int, default=8420, help="Port")
    args = parser.parse_args()

    print(f"Starting agy-bridge on {args.host}:{args.port}...")
    uvicorn.run(app, host=args.host, port=args.port)
