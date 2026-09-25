#!/usr/bin/env python3
"""devOS Antigravity bridge: drive the `agy` CLI from the Tab5.

The bridge runs next to your code (on your computer), starts Antigravity's
headless mode

    agy --input-format stream-json --output-format stream-json \
        --dangerously-skip-permissions [--conversation ID] [--model M]

in your project folder, and relays it to the Tab5 over one WebSocket (`/ws`).
Tool permissions are NOT skipped: a PreToolUse hook (devos_hook.sh, installed
into ~/.gemini/config/hooks.json while the bridge runs) asks the bridge before
every tool call. Read-only tools are allowed; everything else (commands, file
edits, browsing, MCP calls...) waits for Allow / Deny on the Tab5, and is
denied if the bridge is unreachable. Outside bridge sessions the hook is a
no-op, so your normal `agy` use is unaffected.

Conversations are shared with antigravity.google.com: the Tab5 lists the
same history `agy remote-control start` shows there (read from agy's local
store, see agy_store.py), can open any of them, and continues it in that
conversation's own folder. Sessions run with --remote-control, so what the
Tab5 does is live on the website too, and a conversation moved on from the
website refreshes on the Tab5. Prompts are shared with agy's up-arrow history.

Setup and options: see README.md next to this file.

Run (from your project folder, which agy should already trust):
    python3 tools/agy_bridge/bridge_server.py --psk <secret>
    python3 tools/agy_bridge/bridge_server.py --workspace ~/dev/app --resume
    python3 tools/agy_bridge/bridge_server.py --demo      # scripted, no agy

Protocol: JSON objects with a "type" field, one per WebSocket message.
  Tab5 -> bridge:
    HELLO {token, client}          PROMPT {text, command}
    PERMISSION_REPLY {id, allow, always}
    LIST (resend CONVERSATIONS)    OPEN {id} (switch to that conversation)
    QUESTION_REPLY {id, choice (-1 = skip), text (optional typed answer)}
    NEW                            ABORT                  PING
  Bridge -> Tab5:
    RESET (clear the view; the conversation so far is replayed after it)
    WELCOME {conversation_id, model, workspace, subagents:[{name,state}]}
    USER {text} (a prompt sent from another screen, or replayed)
    TOKEN {text}                   THINKING {text}
    TOOL {id, name, state, detail} (same id = update: running -> done/error)
    DIFF {file, hunk}              ARTIFACT {name, kind, text}
    PERMISSION {id, text, preview (optional diff of a pending edit)}
    QUESTION {id, prompt, choices:[...]}
    RESOLVED {id} (that permission/question is settled: close its dialog)
    SUBAGENTS {agents}             STATUS {state: busy|idle}
    USAGE {input, output, total}   ERROR {message}        PONG
    CONVERSATIONS {instance, items:[{id, title, ws, age, steps, busy}]} (newest first)
    PROMPTS {items:[...]} (recent prompts, oldest first: the up-arrow history)
"""

import argparse
import asyncio
import difflib
import json
import os
import secrets
import signal
import sys
import time
import uuid
from pathlib import Path

try:
    from websockets.asyncio.server import serve
    from websockets.exceptions import ConnectionClosed
except ImportError:  # pragma: no cover
    sys.exit("The bridge needs the `websockets` package: pip install websockets")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import agy_store  # noqa: E402  (next to this file)
HOOKS_FILE = Path.home() / ".gemini" / "config" / "hooks.json"
HOOK_NAME = "devos-tab5-bridge"
BRAIN_DIR = Path.home() / ".gemini" / "antigravity-cli" / "brain"
AGY_SETTINGS = Path.home() / ".gemini" / "antigravity-cli" / "settings.json"

# Tools that only read local state or manage the agent itself: never asked.
AUTO_ALLOW = {
    "view_file", "list_dir", "grep_search", "find_by_name", "command_status",
    "list_resources", "read_resource", "list_permissions", "wait", "wait_5_seconds",
    "finish", "manage_task", "manage_inbox", "invoke_subagent", "define_subagent",
    "manage_subagents", "schedule", "search_web", "list_browser_pages", "read_browser_page",
    "capture_browser_screenshot", "capture_browser_console_logs", "browser_get_dom",
    "browser_list_network_requests", "browser_get_network_request",
}
EDIT_TOOLS = {"write_to_file", "replace_file_content", "multi_replace_file_content", "sed_file", "notebook_edit"}
PERMISSION_TIMEOUT = 600          # seconds a tool waits for the Tab5
HISTORY_MAX = 400                 # events replayed to a Tab5 that (re)connects


def wire(msg):
    """JSON for the Tab5: raw UTF-8 rather than \\u escapes."""
    return json.dumps(msg, ensure_ascii=False)


def log(*a):
    print("[agy-bridge]", *a, flush=True)


# --------------------------------------------------------------------------
# Hook installation (~/.gemini/config/hooks.json)
# --------------------------------------------------------------------------
def install_hook():
    HOOKS_FILE.parent.mkdir(parents=True, exist_ok=True)
    existed = HOOKS_FILE.exists()
    data = {}
    if existed:
        try:
            data = json.loads(HOOKS_FILE.read_text() or "{}")
        except ValueError:
            sys.exit(f"{HOOKS_FILE} is not valid JSON; fix it or move it aside first")
    data[HOOK_NAME] = {
        "PreToolUse": [{
            "matcher": "*",
            "hooks": [{"type": "command", "command": str(HERE / "devos_hook.sh"),
                       "timeout": PERMISSION_TIMEOUT + 60}],
        }]
    }
    tmp = HOOKS_FILE.with_suffix(".json.devos-tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n")
    tmp.replace(HOOKS_FILE)
    return existed


def remove_hook(existed_before):
    try:
        data = json.loads(HOOKS_FILE.read_text() or "{}")
    except (OSError, ValueError):
        return
    data.pop(HOOK_NAME, None)
    if not data and not existed_before:
        HOOKS_FILE.unlink(missing_ok=True)
    else:
        HOOKS_FILE.write_text(json.dumps(data, indent=2) + "\n")


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def short(s, n=160):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "..."


def target_file(args):
    for k in ("TargetFile", "AbsolutePath", "FilePath", "File", "Path", "path"):
        if isinstance(args.get(k), str):
            return args[k]
    return ""


WORKSPACE = ""                     # set in main(); paths inside it are shown relative


def rel(path):
    if WORKSPACE and isinstance(path, str) and path.startswith(WORKSPACE.rstrip("/") + "/"):
        return path[len(WORKSPACE.rstrip("/")) + 1:]
    return path


def describe_tool(name, args):
    """Human text for a tool call (permission prompts and tool cards)."""
    if name in ("run_command", "send_command_input"):
        cmd = args.get("CommandLine") or args.get("Input") or ""
        cwd = args.get("Cwd") or ""
        cwd = rel(cwd)
        return f"$ {short(cmd, 300)}" + (f"\n(in {cwd})" if cwd and cwd not in (".", WORKSPACE) else "")
    if name in EDIT_TOOLS:
        verb = "Create/overwrite" if name == "write_to_file" else "Edit"
        return f"{verb} {rel(target_file(args)) or '?'}"
    if name in ("open_browser_url", "read_url_content"):
        return f"Open {args.get('Url') or args.get('url') or '?'}"
    if name == "call_mcp_tool":
        return f"MCP {args.get('ServerName', '?')}/{args.get('ToolName', '?')}"
    if name == "view_file":
        return f"Read {rel(target_file(args))}"
    if name in ("grep_search", "find_by_name"):
        return f"Search {args.get('Query') or args.get('Pattern') or args.get('SearchPattern') or ''}"
    if name == "list_dir":
        return f"List {rel(args.get('DirectoryPath') or target_file(args))}"
    summary = args.get("toolSummary") or args.get("toolAction")
    if summary:
        return short(summary, 200)
    rest = {k: v for k, v in args.items() if k not in ("toolAction", "toolSummary", "WaitMsBeforeAsync")}
    return short(json.dumps(rest), 200) if rest else ""


def edit_preview(name, args):
    """Diff of what an edit tool call is about to do, for the Allow dialog."""
    path = target_file(args)
    before = read_small(path) if path else None
    after = None
    if name == "write_to_file":
        after = str(args.get("CodeContent") or "")
    elif name in ("replace_file_content", "multi_replace_file_content"):
        chunks = args.get("ReplacementChunks")
        chunks = chunks if isinstance(chunks, list) else [args]
        after = before
        for c in chunks:
            t = c.get("TargetContent") if isinstance(c, dict) else None
            r = c.get("ReplacementContent") if isinstance(c, dict) else None
            if after is None or not isinstance(t, str) or not isinstance(r, str) or t not in after:
                after = None
                break
            after = after.replace(t, r) if c.get("AllowMultiple") else after.replace(t, r, 1)
        if after is None:           # can't apply it here: show the chunks as-is
            out = []
            for c in chunks:
                if isinstance(c, dict):
                    out += ["@@"] + ["-" + l for l in str(c.get("TargetContent") or "").splitlines()]
                    out += ["+" + l for l in str(c.get("ReplacementContent") or "").splitlines()]
            return "\n".join(out)[:6000]
    if after is None:
        return ""
    return file_diff(path, before or "", after)[:6000]


def always_key(name, args):
    if name in ("run_command", "send_command_input"):
        return f"cmd:{args.get('CommandLine') or args.get('Input')}"
    if name in EDIT_TOOLS:
        return f"edit:{target_file(args)}"
    return f"tool:{name}"


def read_small(path, limit=512 * 1024):
    try:
        p = Path(path)
        if not p.is_file():
            return ""
        if p.stat().st_size > limit:
            return None
        return p.read_text(errors="replace")
    except OSError:
        return None


def file_diff(path, before, after):
    """Unified diff of a file before/after an edit tool ran."""
    name = rel(path)
    return "\n".join(difflib.unified_diff(before.splitlines(), after.splitlines(),
                                         fromfile="a/" + name, tofile="b/" + name, lineterm="", n=2))


def default_model():
    try:
        return json.loads(AGY_SETTINGS.read_text()).get("model", "")
    except (OSError, ValueError):
        return ""


# --------------------------------------------------------------------------
# Bridge
# --------------------------------------------------------------------------
class Bridge:
    def __init__(self, args):
        self.args = args
        self.default_workspace = str(Path(args.workspace).expanduser().resolve())
        self.workspace = self.default_workspace
        self.model = args.model or default_model()
        self.clients = set()                    # authenticated websockets
        self.history = []                       # replayed to late joiners
        self.proc = None
        self.conversation_id = args.conversation or ""
        self.resume = bool(args.resume)
        self.busy = False
        self.pending = {}                       # id -> {"future", "msg"}
        self.always = set()
        self.tools = {}                         # step index -> tool card id
        self.subagents = {}                     # name -> state
        self.artifact_mtimes = {}
        self.snapshots = {}                     # step index -> (file, content before the edit)
        self.sock_path = ""
        self.stopping = False
        self.known_steps = {}                   # conversation id -> step count last shown
        self.own_turn_until = 0.0               # store updates until then are our own turn
        self.list_stamp = 0.0
        self.instance = ""                      # remote-control instance name

    # ---- fan-out ----
    async def emit(self, msg, record=True):
        if record and msg.get("type") not in ("PONG", "PERMISSION", "QUESTION", "RESET"):
            self.remember(msg)
        data = wire(msg)
        for ws in list(self.clients):
            try:
                await ws.send(data)
            except ConnectionClosed:
                self.clients.discard(ws)

    def remember(self, msg):
        """Keep the replay history compact: streamed text is merged, and a
        tool card's updates replace its earlier entry."""
        h, t = self.history, msg.get("type")
        if t in ("TOKEN", "THINKING") and h and h[-1].get("type") == t and len(h[-1]["text"]) < 4000:
            h[-1] = dict(h[-1], text=h[-1]["text"] + msg.get("text", ""))
            return
        if t == "TOOL" and msg.get("id"):
            for i in range(len(h) - 1, max(-1, len(h) - 200), -1):
                if h[i].get("type") == "TOOL" and h[i].get("id") == msg["id"]:
                    h[i] = msg
                    return
        h.append(msg)
        del h[:-HISTORY_MAX]

    async def status(self, busy):
        self.busy = busy
        await self.emit({"type": "STATUS", "state": "busy" if busy else "idle"})
        await self.emit({"type": "SUBAGENTS", "agents": self.agents()})

    def agents(self):
        main = [{"name": "agent", "state": "running" if self.busy else "idle"}]
        return (main + [{"name": n, "state": s} for n, s in self.subagents.items()])[:8]

    def welcome(self):
        return {"type": "WELCOME", "conversation_id": self.conversation_id, "model": self.model,
                "workspace": self.workspace, "subagents": self.agents()}

    def set_workspace(self, path):
        global WORKSPACE
        self.workspace = path if path and Path(path).is_dir() else self.default_workspace
        WORKSPACE = self.workspace

    # ---- agy process ----
    async def start_agy(self):
        if self.args.demo:
            return
        cmd = ["agy", "--input-format", "stream-json", "--output-format", "stream-json",
               "--dangerously-skip-permissions", "--add-dir", self.workspace]
        if self.conversation_id:
            cmd += ["--conversation", self.conversation_id]
        elif self.resume:
            cmd += ["--continue"]
        if self.args.remote_control:
            cmd += ["--remote-control"]
        if self.args.model:
            cmd += ["--model", self.args.model]
        env = dict(os.environ, DEVOS_AGY_BRIDGE_SOCK=self.sock_path)
        self.proc = await asyncio.create_subprocess_exec(
            *cmd, cwd=self.workspace, env=env, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=16 * 1024 * 1024)
        what = (f"resuming {self.conversation_id}" if self.conversation_id
                else "continuing the latest conversation" if self.resume else "new conversation")
        log(f"started agy ({what})")
        asyncio.create_task(self.read_stdout(self.proc))
        asyncio.create_task(self.read_stderr(self.proc))

    async def stop_agy(self):
        p, self.proc = self.proc, None
        if not p or p.returncode is not None:
            return
        try:
            p.send_signal(signal.SIGINT)
            await asyncio.wait_for(p.wait(), 5)
        except (asyncio.TimeoutError, ProcessLookupError):
            p.kill()

    async def read_stderr(self, proc):
        while True:
            line = await proc.stderr.readline()
            if not line:
                break
            text = line.decode(errors="replace").rstrip()
            if text:
                log("agy:", text[:300])

    async def read_stdout(self, proc):
        while True:
            line = await proc.stdout.readline()
            if not line:
                break
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            try:
                await self.on_agy_event(ev)
            except Exception as e:  # never let one odd event kill the reader
                log("event error:", repr(e))
        if proc is self.proc and not self.stopping:
            log("agy exited; it restarts with the next prompt")
            self.proc = None
            if self.busy:
                await self.status(False)

    async def on_agy_event(self, ev):
        kind = ev.get("event")
        if kind == "init":
            cid = ev.get("conversation_id") or ""
            if cid and cid != self.conversation_id:
                self.conversation_id = cid
                self.artifact_mtimes = {}
            await self.emit(self.welcome())
        elif kind == "step_update":
            await self.on_step(ev.get("step_update") or {})
        elif kind == "result":
            r = ev.get("result") or {}
            if r.get("status") not in (None, "SUCCESS"):
                await self.emit({"type": "ERROR", "message": "Antigravity: " + short(r.get("error") or r.get("status"), 240)})
            for d in r.get("denied_actions") or []:
                await self.emit({"type": "ERROR", "message": f"Denied: {d.get('display_name') or d.get('action')}"})
            u = r.get("usage") or {}
            await self.emit({"type": "USAGE", "input": u.get("input_tokens", 0), "output": u.get("output_tokens", 0),
                             "total": u.get("total_tokens", 0)})
            for n in list(self.subagents):
                self.subagents[n] = "idle"
            self.own_turn_until = time.time() + 15
            await self.status(False)
            await self.scan_artifacts()

    async def on_step(self, u):
        stype = u.get("step_type")
        state = u.get("state")
        if u.get("thinking_delta"):
            await self.emit({"type": "THINKING", "text": u["thinking_delta"]})
        if stype == "agent_response":
            if u.get("text_delta"):
                await self.emit({"type": "TOKEN", "text": u["text_delta"]})
        elif stype == "tool":
            info = u.get("tool_info") or {}
            name = u.get("tool_name") or info.get("name") or "tool"
            params = info.get("parameters") or {}
            tid = f"{self.conversation_id[:8]}-{u.get('step_index')}"
            st = {"ACTIVE": "running", "DONE": "done", "ERROR": "error"}.get(state, (state or "").lower())
            detail = describe_tool(name, params)
            err = (info.get("error") or {}).get("message")
            if err:
                detail += "\n" + short(err.split("\nDo not attempt")[0], 300)
            await self.emit({"type": "TOOL", "id": tid, "name": name, "state": st, "detail": detail})
            if name in ("invoke_subagent", "define_subagent"):
                subs = params.get("Subagents") if isinstance(params.get("Subagents"), list) else [params]
                for i, sa in enumerate(subs):
                    sa = sa if isinstance(sa, dict) else {}
                    label = sa.get("Name") or sa.get("SubagentName") or short(sa.get("Prompt") or "", 28) or f"subagent {i + 1}"
                    self.subagents[label] = "running" if st == "running" else "idle"
                while len(self.subagents) > 7:
                    self.subagents.pop(next(iter(self.subagents)))
                await self.emit({"type": "SUBAGENTS", "agents": self.agents()})
            if st in ("done", "error") and name in EDIT_TOOLS:
                snap = self.snapshots.pop(u.get("step_index"), None)
                if snap and st == "done":
                    after = read_small(snap[0])
                    if after is not None and snap[1] is not None and after != snap[1]:
                        hunk = file_diff(snap[0], snap[1], after)
                        if hunk:
                            await self.emit({"type": "DIFF", "file": rel(snap[0]), "hunk": hunk[:6000]})
            if st in ("done", "error"):
                await self.scan_artifacts()

    async def scan_artifacts(self):
        """Antigravity writes plans/tasks/walkthroughs as markdown artifacts."""
        if not self.conversation_id:
            return
        d = BRAIN_DIR / self.conversation_id
        try:
            files = [p for p in d.iterdir() if p.suffix == ".md" and p.is_file()]
        except OSError:
            return
        for p in files:
            m = p.stat().st_mtime
            if self.artifact_mtimes.get(p.name) == m:
                continue
            self.artifact_mtimes[p.name] = m
            try:
                text = p.read_text(errors="replace")
            except OSError:
                continue
            await self.emit({"type": "ARTIFACT", "name": p.name, "kind": "markdown", "text": text[:12000]})

    async def prompt(self, text, command=""):
        full = f"{command} {text}".strip() if command else text
        if self.args.demo:
            asyncio.create_task(self.demo_turn(full))
            return
        if not self.proc or self.proc.returncode is not None:
            try:
                await self.start_agy()
            except OSError as e:                    # agy missing, folder gone...
                log("could not start agy:", e)
                await self.emit({"type": "ERROR", "message": f"Couldn't start agy on the computer: {e}"})
                return
        await self.status(True)
        line = json.dumps({"event": "user", "message": {"content": full}}) + "\n"
        self.proc.stdin.write(line.encode())
        await self.proc.stdin.drain()

    async def abort(self):
        """Stop the running turn; the conversation continues with the next prompt."""
        for p in list(self.pending.values()):
            if not p["future"].done():
                p["future"].set_result({"allow": False, "choice": -1, "aborted": True})
        await self.stop_agy()
        await self.status(False)
        await self.emit({"type": "TOKEN", "text": "\n(stopped)\n"})

    async def stop_turn(self):
        """End any running turn without a "(stopped)" note (switching away)."""
        for p in list(self.pending.values()):
            if not p["future"].done():
                p["future"].set_result({"allow": False, "choice": -1, "aborted": True})
        await self.stop_agy()
        if self.busy:
            await self.status(False)

    async def new_conversation(self):
        await self.stop_turn()
        self.conversation_id = ""
        self.resume = False
        self.set_workspace(self.default_workspace)
        self.history.clear()
        self.subagents.clear()
        self.snapshots.clear()
        self.artifact_mtimes = {}
        await self.replay_all()

    # ---- shared history (antigravity.google.com / agy's store) ----
    def conversations_msg(self):
        items = [{k: c[k] for k in ("id", "title", "ws", "age", "steps", "busy")}
                 for c in agy_store.list_conversations(30)]
        return {"type": "CONVERSATIONS", "instance": self.instance, "items": items}

    def load(self, cid):
        """Make `cid` the current conversation, its transcript the history."""
        info = agy_store.conversation(cid)
        self.conversation_id = cid
        self.resume = False
        self.set_workspace(info["workspace"] if info else "")
        self.history = agy_store.load_transcript(cid, describe_tool, HISTORY_MAX)
        self.known_steps[cid] = info["steps"] if info else 0
        self.subagents.clear()
        self.snapshots.clear()
        self.artifact_mtimes = {}
        return info

    async def open_conversation(self, cid):
        if not cid or not (agy_store.CONVERSATIONS_DIR / f"{cid}.db").is_file():
            await self.emit({"type": "ERROR", "message": "That conversation isn't on this computer."}, record=False)
            return
        if cid == self.conversation_id and self.history:
            await self.replay_all()
            return
        await self.stop_turn()
        info = self.load(cid)
        log(f"opened conversation {cid[:8]} ({info['title'] if info else '?'}) in {self.workspace}")
        await self.scan_artifacts()
        await self.replay_all()

    async def replay_all(self):
        for ws in list(self.clients):
            try:
                await self.replay(ws)
            except ConnectionClosed:
                self.clients.discard(ws)

    async def watch_store(self):
        """Follow the store: new/renamed conversations refresh the Tab5's
        list, and the open conversation reloads when it moved on elsewhere
        (antigravity.google.com, the CLI)."""
        while True:
            await asyncio.sleep(3)
            stamp = agy_store.summaries_stamp()
            if stamp == self.list_stamp or not self.clients:
                continue
            self.list_stamp = stamp
            msg = self.conversations_msg()
            await self.emit(msg, record=False)
            cid = self.conversation_id
            cur = next((c for c in msg["items"] if c["id"] == cid), None)
            if not cur:
                continue
            seen = self.known_steps.get(cid, cur["steps"])
            self.known_steps[cid] = cur["steps"]
            if cur["steps"] > seen and not self.busy and time.time() > self.own_turn_until:
                log(f"conversation {cid[:8]} moved on elsewhere; reloading")
                self.load(cid)
                await self.scan_artifacts()
                await self.replay_all()

    # ---- permission hook (Unix socket) ----
    async def on_hook(self, reader, writer):
        try:
            line = await reader.readline()
            payload = json.loads(line or b"{}")
            decision = await self.decide(payload)
        except Exception as e:
            decision = {"decision": "deny", "reason": f"devOS bridge error: {e}"}
        writer.write((json.dumps(decision) + "\n").encode())
        try:
            await writer.drain()
        finally:
            writer.close()

    async def decide(self, payload):
        call = payload.get("toolCall") or {}
        name = call.get("name") or ""
        args = call.get("args") or {}
        if name == "ask_question":
            return await self.ask_question(args)
        if name in ("ask_permission", "ask_custom_permission"):
            ok = await self.ask_tab5(name, args, "The agent asks for permission:\n" + describe_tool(name, args))
            return {"decision": "allow" if ok else "deny", "reason": "decided on the Tab5"}
        if self.args.yolo or name in AUTO_ALLOW or always_key(name, args) in self.always:
            ok = True
        else:
            preview = edit_preview(name, args) if name in EDIT_TOOLS else ""
            ok = await self.ask_tab5(name, args, describe_tool(name, args) or name, preview)
        if ok:
            if name in EDIT_TOOLS and target_file(args):
                # remember the file as it was, to diff it once the edit is done
                self.snapshots[payload.get("stepIdx")] = (target_file(args), read_small(target_file(args)))
            return {"decision": "allow", "reason": "approved on the Tab5"}
        return {"decision": "deny", "reason": "The user denied this on their Tab5. Do not retry it or work around it."}

    async def ask_tab5(self, name, args, text, preview=""):
        pid = "perm-" + uuid.uuid4().hex[:8]
        fut = asyncio.get_running_loop().create_future()
        msg = {"type": "PERMISSION", "id": pid, "text": f"{name}\n{text}"}
        if preview:
            msg["preview"] = preview
        self.pending[pid] = {"future": fut, "msg": msg}
        log("permission?", name, short(text, 120))
        await self.emit(msg, record=False)
        try:
            res = await asyncio.wait_for(fut, PERMISSION_TIMEOUT)
        except asyncio.TimeoutError:
            res = {"allow": False}
        finally:
            self.pending.pop(pid, None)
            await self.emit({"type": "RESOLVED", "id": pid}, record=False)
        if res.get("allow") and res.get("always"):
            self.always.add(always_key(name, args))
        log("permission", "allowed" if res.get("allow") else "denied", name)
        return bool(res.get("allow"))

    async def ask_question(self, args):
        prompt, choices = "", []
        qs = args.get("Questions") or args.get("questions")
        q = qs[0] if isinstance(qs, list) and qs else args
        if isinstance(q, dict):
            prompt = q.get("Question") or q.get("question") or q.get("Prompt") or q.get("prompt") or ""
            opts = q.get("Options") or q.get("options") or q.get("Choices") or q.get("choices") or []
            for o in opts if isinstance(opts, list) else []:
                choices.append(o if isinstance(o, str) else (o.get("Label") or o.get("label") or o.get("text") or json.dumps(o)))
        if not prompt:
            prompt = short(json.dumps(args), 300)
        qid = "q-" + uuid.uuid4().hex[:8]
        fut = asyncio.get_running_loop().create_future()
        msg = {"type": "QUESTION", "id": qid, "prompt": prompt, "choices": [short(c, 120) for c in choices[:4]]}
        self.pending[qid] = {"future": fut, "msg": msg}
        await self.emit(msg, record=False)
        try:
            res = await asyncio.wait_for(fut, PERMISSION_TIMEOUT)
        except asyncio.TimeoutError:
            res = {"choice": -1}
        finally:
            self.pending.pop(qid, None)
            await self.emit({"type": "RESOLVED", "id": qid}, record=False)
        c = res.get("choice", -1)
        typed = str(res.get("text") or "").strip()[:500]
        if typed or (isinstance(c, int) and 0 <= c < len(choices)):
            answer = typed or choices[c]
            await self.emit({"type": "TOKEN", "text": f"\n(answered: {answer})\n"})
            return {"decision": "deny", "reason": f"The user already answered this question on their Tab5: "
                                                  f"\"{answer}\". Use that answer and do not ask again."}
        return {"decision": "deny", "reason": "Nobody answered on the Tab5; continue with your best judgement."}

    # ---- WebSocket ----
    async def on_ws(self, ws):
        authed = not self.args.psk
        peer = ws.remote_address[0] if ws.remote_address else "?"
        try:
            if authed:
                await self.join(ws)
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except ValueError:
                    await ws.send(wire({"type": "ERROR", "message": "bad json"}))
                    continue
                t = msg.get("type", "")
                if t == "HELLO":
                    if self.args.psk and not secrets.compare_digest(str(msg.get("token", "")), self.args.psk):
                        await ws.send(wire({"type": "ERROR", "message": "Wrong bridge token"}))
                        log("rejected client", peer, "(wrong token)")
                        return
                    if not authed:
                        authed = True
                        await self.join(ws)
                elif not authed:
                    await ws.send(wire({"type": "ERROR", "message": "Send HELLO with the bridge token first"}))
                elif t == "PING":
                    await ws.send(wire({"type": "PONG"}))
                elif t == "PROMPT":
                    text = str(msg.get("text", ""))[:16000]
                    command = str(msg.get("command", ""))[:32]
                    log("prompt:", short(text, 80))
                    # the sender shows its own words; replay + other screens need them
                    user = {"type": "USER", "text": f"{command} {text}".strip()}
                    self.remember(user)
                    if not self.args.demo:
                        agy_store.add_prompt(user["text"], self.workspace, self.conversation_id)
                    for other in list(self.clients - {ws}):
                        try:
                            await other.send(wire(user))
                        except ConnectionClosed:
                            self.clients.discard(other)
                    await self.prompt(text, command)
                elif t in ("PERMISSION_REPLY", "QUESTION_REPLY"):
                    p = self.pending.get(str(msg.get("id")))
                    if p and not p["future"].done():
                        p["future"].set_result(msg)
                elif t == "NEW":
                    await self.new_conversation()
                elif t == "LIST":
                    await ws.send(wire(self.conversations_msg()))
                elif t == "OPEN":
                    await self.open_conversation(str(msg.get("id", ""))[:64])
                elif t == "ABORT":
                    await self.abort()
                else:
                    await ws.send(wire({"type": "ERROR", "message": f"unknown message {t}"}))
        except ConnectionClosed:
            pass
        finally:
            self.clients.discard(ws)
            if authed:
                log("Tab5 disconnected:", peer)

    async def join(self, ws):
        self.clients.add(ws)
        log("Tab5 connected:", ws.remote_address[0] if ws.remote_address else "?")
        await self.replay(ws)
        await ws.send(wire(self.conversations_msg()))
        await ws.send(wire({"type": "PROMPTS", "items": agy_store.recent_prompts(40)}))

    async def replay(self, ws):
        await ws.send(wire({"type": "RESET"}))
        await ws.send(wire(self.welcome()))
        for m in self.history:                  # catch up on the conversation
            await ws.send(wire(m))
        await ws.send(wire({"type": "STATUS", "state": "busy" if self.busy else "idle"}))
        for p in self.pending.values():         # questions still waiting for an answer
            await ws.send(wire(p["msg"]))

    # ---- demo mode ----
    async def demo_turn(self, text):
        await self.status(True)
        tid = "demo-" + uuid.uuid4().hex[:4]
        await self.emit({"type": "THINKING", "text": f"Planning: {short(text, 100)}"})
        await asyncio.sleep(0.4)
        await self.emit({"type": "TOOL", "id": tid + "a", "name": "view_file", "state": "done",
                         "detail": "Read src/main.c"})
        ok = await self.ask_tab5("run_command", {"CommandLine": "make test"}, "$ make test")
        await self.emit({"type": "TOOL", "id": tid + "b", "name": "run_command", "state": "done" if ok else "error",
                         "detail": "$ make test" + ("\n12 passed" if ok else "\ndenied on the Tab5")})
        for w in ["Demo ", "reply: ", "all ", "tests ", "pass." if ok else "skipped the tests."]:
            await self.emit({"type": "TOKEN", "text": w})
            await asyncio.sleep(0.15)
        await self.emit({"type": "DIFF", "file": "src/main.c", "hunk": "@@ -1,2 +1,3 @@\n+#include <stdio.h>\n int main(void)\n"})
        await self.emit({"type": "ARTIFACT", "name": "implementation_plan.md", "kind": "markdown",
                         "text": "# Plan\n\n- [x] Reproduce\n- [ ] Fix\n"})
        await self.status(False)


async def reachable_addresses():
    """Names/IPs the Tab5 can use to reach this computer (best effort)."""
    out = []
    try:
        p = await asyncio.create_subprocess_exec("tailscale", "status", "--self", "--json",
                                                 stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        raw, _ = await asyncio.wait_for(p.communicate(), 5)
        me = json.loads(raw or b"{}").get("Self") or {}
        if me.get("DNSName"):
            out.append(me["DNSName"].rstrip("."))
        out += [ip for ip in me.get("TailscaleIPs") or [] if "." in ip]
    except (OSError, ValueError, asyncio.TimeoutError):
        pass
    import socket
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))              # picks the LAN interface; sends nothing
            out.append(s.getsockname()[0])
    except OSError:
        pass
    out.append(socket.gethostname())
    return list(dict.fromkeys(a for a in out if a))


async def remote_control_instance():
    """This machine's name on antigravity.google.com ("" if not registered)."""
    try:
        p = await asyncio.create_subprocess_exec("agy", "remote-control", "status", stdout=asyncio.subprocess.PIPE,
                                                 stderr=asyncio.subprocess.DEVNULL)
        out, _ = await asyncio.wait_for(p.communicate(), 10)
    except (OSError, asyncio.TimeoutError):
        return ""
    for line in out.decode(errors="replace").splitlines():
        if line.startswith("Instance name:"):
            return line.split(":", 1)[1].split("(")[0].strip()
    return ""


async def main():
    ap = argparse.ArgumentParser(description="devOS Antigravity bridge (Tab5 <-> agy)")
    ap.add_argument("--host", default="0.0.0.0", help="listen address (default all interfaces)")
    ap.add_argument("--port", type=int, default=8420)
    ap.add_argument("--psk", default=os.environ.get("DEVOS_BRIDGE_PSK", ""),
                    help="shared token the Tab5 must send (generated if omitted)")
    ap.add_argument("--workspace", default=".", help="project folder agy works in (default: here)")
    ap.add_argument("--model", default="", help="agy model (default: your agy setting)")
    ap.add_argument("--conversation", default="", help="resume this conversation id")
    ap.add_argument("--resume", action="store_true", help="continue the most recent conversation in --workspace")
    ap.add_argument("--no-remote-control", dest="remote_control", action="store_false",
                    help="don't also show the session on antigravity.google.com")
    ap.add_argument("--yolo", action="store_true", help="allow every tool without asking (not recommended)")
    ap.add_argument("--no-psk", action="store_true", help="allow clients without a token (loopback only)")
    ap.add_argument("--demo", action="store_true", help="scripted replies, no agy needed")
    args = ap.parse_args()

    if not args.psk and not args.no_psk:
        args.psk = secrets.token_urlsafe(12)
        log(f"No --psk given; this run's token is: {args.psk}")
    if args.no_psk and args.host not in ("127.0.0.1", "localhost", "::1"):
        sys.exit("--no-psk is only allowed with --host 127.0.0.1")

    if not args.demo and not Path(args.workspace).expanduser().is_dir():
        sys.exit(f"--workspace {args.workspace}: no such folder (use your project's folder)")
    bridge = Bridge(args)
    bridge.set_workspace(bridge.default_workspace)
    if not args.demo:
        bridge.instance = await remote_control_instance()
        start = args.conversation
        if args.resume and not start:
            here = [c for c in agy_store.list_conversations(200) if c["workspace"] == bridge.workspace]
            start = here[0]["id"] if here else ""
        if start:
            info = bridge.load(start)
            log(f"resuming {start[:8]} ({info['title'] if info else 'not in the store yet'})")
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    bridge.sock_path = os.path.join(runtime, f"devos-agy-bridge-{os.getpid()}.sock")
    old_umask = os.umask(0o077)                     # socket readable by this user only
    hook_server = await asyncio.start_unix_server(bridge.on_hook, path=bridge.sock_path)
    os.umask(old_umask)

    existed = False if args.demo else install_hook()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    listening = False
    try:
        async with serve(bridge.on_ws, args.host, args.port, max_size=4 * 1024 * 1024, ping_interval=20):
            listening = True
            log(f"listening on ws://{args.host}:{args.port}/ws  workspace={bridge.workspace}  "
                f"model={bridge.model or 'default'}  demo={args.demo}")
            if args.host in ("127.0.0.1", "localhost", "::1"):
                log("only this computer can connect (--host 127.0.0.1); use the default host for the Tab5")
            else:
                log("On the Tab5: Antigravity > Set up connection, then enter")
                log("  address:  " + "  or  ".join(await reachable_addresses()))
                log(f"  port:     {args.port}")
                log(f"  token:    {args.psk or '(leave empty)'}")
            if not args.demo and bridge.conversation_id:
                await bridge.start_agy()
                await bridge.scan_artifacts()
            watcher = asyncio.create_task(bridge.watch_store()) if not args.demo else None
            await stop.wait()
            if watcher:
                watcher.cancel()
    except OSError as e:
        if listening:
            raise
        log(f"can't listen on {args.host}:{args.port}: {e.strerror} "
            "(is another bridge already running? pick another --port)")
    finally:
        bridge.stopping = True
        await bridge.stop_agy()
        hook_server.close()
        try:
            os.unlink(bridge.sock_path)
        except OSError:
            pass
        if not args.demo:
            remove_hook(existed)
        log("stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
