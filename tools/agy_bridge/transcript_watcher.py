"""
Realtime parser and file watcher for Antigravity session transcript logs.
Path: ~/.gemini/antigravity-cli/brain/<conversation-id>/.system_generated/logs/transcript.jsonl
"""

import os
import json
import time
from typing import Generator, Dict, Any, Optional

def find_latest_transcript() -> Optional[str]:
    base_dir = os.path.expanduser("~/.gemini/antigravity-cli/brain")
    if not os.path.exists(base_dir):
        return None

    conversations = [
        os.path.join(base_dir, d) for d in os.listdir(base_dir)
        if os.path.isdir(os.path.join(base_dir, d))
    ]
    if not conversations:
        return None

    # Sort by modification time
    conversations.sort(key=lambda x: os.path.getmtime(x), reverse=True)
    latest = conversations[0]
    log_file = os.path.join(latest, ".system_generated", "logs", "transcript.jsonl")
    if os.path.exists(log_file):
        return log_file
    return None

class TranscriptWatcher:
    def __init__(self, file_path: Optional[str] = None):
        self.file_path = file_path or find_latest_transcript()
        self._last_pos = 0

    def get_events(self) -> Generator[Dict[str, Any], None, None]:
        if not self.file_path or not os.path.exists(self.file_path):
            return

        with open(self.file_path, "r", encoding="utf-8") as f:
            f.seek(self._last_pos)
            for line in f:
                line = line.strip()
                if line:
                    try:
                        data = json.loads(line)
                        yield data
                    except json.JSONDecodeError:
                        continue
            self._last_pos = f.tell()
