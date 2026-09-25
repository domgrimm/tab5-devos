"""Read Antigravity's local conversation store (read-only).

This is the history antigravity.google.com shows for this machine when
`agy remote-control start` is running: the daemon and the CLI keep every
conversation under ~/.gemini/antigravity-cli/

    conversation_summaries.db   one row per conversation (title, workspace, ...)
    conversations/<id>.db       the steps of each conversation (protobuf blobs)
    history.jsonl               prompts typed so far (the CLI's up-arrow history)

The step blobs have no published schema; the few fields used here were
identified from real conversations:

    step_type 14  user input      field 19.2 = the prompt
    step_type 15  model response  field 20.1 = reply text, 20.3 = thinking
    other types   tool steps      field 5.4 = {1: call id, 2: tool name, 3: args JSON}
    status 3 = done, 7 = error (error_details holds the message)

Anything unrecognised is skipped, so a format change degrades to a shorter
transcript rather than an error.
"""

import json
import sqlite3
import time
from pathlib import Path

AGY_HOME = Path.home() / ".gemini" / "antigravity-cli"
SUMMARIES_DB = AGY_HOME / "conversation_summaries.db"
CONVERSATIONS_DIR = AGY_HOME / "conversations"
HISTORY_FILE = AGY_HOME / "history.jsonl"

STEP_USER = 14
STEP_RESPONSE = 15
STATUS_ERROR = 7


# ---------------------------------------------------------------- protobuf
def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def pb_fields(b):
    """Decode one protobuf message into [(field, wiretype, value)]; None if it isn't one."""
    if not b:
        return []
    out, i = [], 0
    try:
        while i < len(b):
            key, i = _varint(b, i)
            field, wt = key >> 3, key & 7
            if field == 0:
                return None
            if wt == 0:
                v, i = _varint(b, i)
            elif wt == 1:
                v, i = b[i:i + 8], i + 8
            elif wt == 5:
                v, i = b[i:i + 4], i + 4
            elif wt == 2:
                n, i = _varint(b, i)
                v = b[i:i + n]
                if len(v) != n:
                    return None
                i += n
            else:
                return None
            out.append((field, wt, v))
    except IndexError:
        return None
    return out


def pb_get(b, *path):
    """Bytes at a field path (first occurrence at each level), or None."""
    for depth, f in enumerate(path):
        fields = pb_fields(b)
        if not fields:
            return None
        hit = next((v for field, wt, v in fields if field == f and wt == 2), None)
        if hit is None:
            return None
        b = hit
    return b


def pb_text(b, *path):
    v = pb_get(b, *path)
    if v is None:
        return ""
    try:
        return v.decode()
    except UnicodeDecodeError:
        return v.decode(errors="replace")


def pb_strings(b, limit=4):
    """Printable strings anywhere inside a message (for error blobs)."""
    found = []

    def walk(buf, depth):
        fields = pb_fields(buf)
        if not fields or depth > 6:
            return
        for _, wt, v in fields:
            if wt != 2 or len(found) >= limit:
                continue
            try:
                s = v.decode()
                if len(s) > 3 and s.isprintable() and " " in s:
                    found.append(s)
                    continue
            except UnicodeDecodeError:
                pass
            walk(v, depth + 1)

    walk(b or b"", 0)
    return found


# ---------------------------------------------------------------- queries
def _ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=2)


def _workspace_path(uris):
    try:
        first = (json.loads(uris or "[]") or [""])[0]
    except ValueError:
        return ""
    return first[len("file://"):] if first.startswith("file://") else first


def _age(seconds):
    if seconds < 90:
        return "now"
    for unit, n in (("d", 86400), ("h", 3600), ("m", 60)):
        if seconds >= n:
            return f"{int(seconds // n)}{unit}"
    return "now"


def _epoch(ts):
    """'2026-09-25 04:45:11.592+00:00' -> unix seconds."""
    from datetime import datetime
    try:
        ts = ts.replace(" ", "T")
        if "." in ts:                      # Python wants <= 6 fraction digits
            head, rest = ts.split(".", 1)
            n = len(rest) - len(rest.lstrip("0123456789"))
            frac, tz = rest[:n], rest[n:]
            ts = f"{head}.{frac[:6]}{tz}"
        return datetime.fromisoformat(ts).timestamp()
    except (ValueError, AttributeError):
        return 0.0


def list_conversations(limit=30):
    """Top-level conversations, newest first (subagent runs are left out)."""
    try:
        db = _ro(SUMMARIES_DB)
        rows = db.execute(
            "select conversation_id, title, preview, step_count, last_modified_time, workspace_uris, status,"
            " not_fully_idle from conversation_summaries where coalesce(nesting_depth, 0) = 0"
            " and coalesce(parent_conversation_id, '') = '' order by last_modified_time desc limit ?",
            (limit,)).fetchall()
        db.close()
    except sqlite3.Error:
        return []
    now = time.time()
    out = []
    for cid, title, preview, steps, modified, uris, status, not_idle in rows:
        ws = _workspace_path(uris)
        updated = _epoch(modified)
        out.append({
            "id": cid,
            "title": (title or preview or "(untitled)").strip()[:80],
            "workspace": ws,
            "ws": Path(ws).name if ws else "",
            "steps": steps or 0,
            "updated": int(updated),
            "age": _age(now - updated) if updated else "",
            "busy": bool(not_idle) or status == "CASCADE_RUN_STATUS_RUNNING",
        })
    return out


def conversation(cid):
    for c in list_conversations(limit=500):
        if c["id"] == cid:
            return c
    return None


def summaries_stamp():
    """Changes whenever the summaries store is written (db or its WAL)."""
    stamp = 0.0
    for p in (SUMMARIES_DB, SUMMARIES_DB.with_name(SUMMARIES_DB.name + "-wal")):
        try:
            stamp = max(stamp, p.stat().st_mtime)
        except OSError:
            pass
    return stamp


def load_transcript(cid, describe=None, max_events=400):
    """A conversation as bridge events (USER / THINKING / TOKEN / TOOL / ERROR)."""
    path = CONVERSATIONS_DIR / f"{cid}.db"
    if not path.is_file():
        return []
    try:
        db = _ro(path)
        rows = db.execute("select idx, step_type, status, step_payload, error_details from steps order by idx").fetchall()
        db.close()
    except sqlite3.Error:
        return []
    events = []
    last_type = None
    for idx, stype, status, payload, err in rows:
        payload = payload or b""
        if stype == STEP_USER:
            text = pb_text(payload, 19, 2).strip()
            if text:
                events.append({"type": "USER", "text": text})
        elif stype == STEP_RESPONSE:
            thinking = pb_text(payload, 20, 3).strip()
            reply = pb_text(payload, 20, 1).strip()
            if thinking:
                events.append({"type": "THINKING", "text": thinking})
            if reply:
                # two replies in a row would merge into one card: keep them apart
                sep = "\n\n" if last_type == "TOKEN" else ""
                events.append({"type": "TOKEN", "text": sep + reply})
        else:
            call = pb_get(payload, 5, 4)
            name = pb_text(call, 2) if call else ""
            if not name:
                continue
            try:
                args = json.loads(pb_text(call, 3) or "{}")
            except ValueError:
                args = {}
            detail = describe(name, args) if describe else ""
            state = "done"
            if status == STATUS_ERROR:
                state = "error"
                msg = next(iter(pb_strings(err, 1)), "")
                if msg:
                    detail = (detail + "\n" if detail else "") + msg.split("\nDo not attempt")[0][:300]
            events.append({"type": "TOOL", "id": f"{cid[:8]}-{idx}", "name": name, "state": state,
                           "detail": detail})
        if events:
            last_type = events[-1]["type"]
    return events[-max_events:]


def recent_prompts(limit=50):
    """Prompts from history.jsonl, oldest first, without repeats."""
    try:
        lines = HISTORY_FILE.read_text(errors="replace").splitlines()[-limit * 4:]
    except OSError:
        return []
    seen, out = set(), []
    for line in reversed(lines):
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if not isinstance(entry, dict) or entry.get("type") == "slash_command":
            continue
        text = str(entry.get("display") or "").strip()
        if text and text not in seen:
            seen.add(text)
            out.append(text[:500])
        if len(out) >= limit:
            break
    return list(reversed(out))


def add_prompt(text, workspace, conversation_id):
    """Append a Tab5 prompt to history.jsonl so agy's own history has it too."""
    entry = {"display": text, "timestamp": int(time.time() * 1000), "workspace": workspace}
    if conversation_id:
        entry["conversationId"] = conversation_id
    try:
        with HISTORY_FILE.open("a") as f:           # one short O_APPEND write
            f.write(json.dumps(entry, ensure_ascii=False, separators=(",", ":")) + "\n")
    except OSError:
        pass
