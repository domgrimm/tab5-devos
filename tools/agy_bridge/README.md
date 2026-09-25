# Antigravity bridge for the Tab5

The Tab5's **Antigravity** app drives Google Antigravity's CLI (`agy`) on a
computer you choose. `agy` does the work on that computer (in your project
folder); the Tab5 is the screen and keyboard: you type prompts, watch replies
and tool calls stream in, approve or deny commands and file edits, answer the
agent's questions, and read diffs and plans.

The bridge is a small Python server that sits between the two.

## 1. Install the Antigravity CLI

On the computer that has your code:

1. Install `agy`, following the Antigravity CLI docs at
   <https://antigravity.google/docs/cli>.
2. Sign in once by running `agy` in a terminal and following the prompts.
3. Run `agy` once inside your project folder and trust it when asked.

Check it works with `agy --version`.

## 2. Get the bridge

Copy this folder (`tools/agy_bridge` from the devOS repository) to the
computer. It needs Python 3.9+ and one package:

```sh
pip install websockets
```

Files: `bridge_server.py` (the server), `devos_hook.sh` + `devos_hook.py`
(the permission hook), `agy_store.py` (reads agy's conversation history).
Keep them together.

## 3. Launch it

From your project folder:

```sh
python3 /path/to/agy_bridge/bridge_server.py
```

It prints what to enter on the Tab5, for example:

```
[agy-bridge] On the Tab5: Antigravity > Set up connection, then enter
[agy-bridge]   address:  my-pc.tail1234.ts.net  or  100.101.102.103  or  192.168.1.20
[agy-bridge]   port:     8420
[agy-bridge]   token:    Xk3v9bQ2mN8p
```

Leave it running while you use the Tab5. Ctrl+C stops it.

Useful options:

| Option | What it does |
| --- | --- |
| `--workspace ~/dev/app` | Project folder for new conversations (default: the current folder) |
| `--resume` | Reopen the most recent conversation in that folder |
| `--conversation ID` | Reopen a specific conversation |
| `--psk SECRET` | Use a fixed token (otherwise a new one each run; or set `DEVOS_BRIDGE_PSK`) |
| `--port 8420` | Listen on another port |
| `--model NAME` | Use a specific model (default: your agy setting) |
| `--no-remote-control` | Don't also show the session on antigravity.google.com |
| `--demo` | Scripted replies without agy, to test the connection |

To keep it running, start it with `tmux`/`screen`, or as a user service.

## 4. Connect the Tab5

Open **Antigravity** on the Tab5. The first time, it asks for the connection:
enter the address, port and token the bridge printed and press **Connect**.
You can change them later by tapping the session card (top left).

The Tab5 must be able to reach the computer: the same Wi-Fi/LAN, or Tailscale
(enable it in the Tab5's Tailscale app and use the computer's Tailscale name).

## Using it

- **Enter** sends; **Esc** or **Stop** ends the running turn.
- **Up/Down** in the prompt box goes through your earlier prompts.
- **History** (or Sym+O) lists your agy conversations, including the ones
  on antigravity.google.com if you use `agy remote-control`. Open one to
  read it and carry on where it left off.
- Commands, file edits, browsing and MCP calls wait for **Allow once / Deny /
  Always allow**. Edits show the diff first. Read-only tools run without asking.
- The **Diff** pane collects every file change; tap it for full screen.

## How permissions work

The bridge runs `agy` in headless mode with `--dangerously-skip-permissions`,
but adds a PreToolUse hook to `~/.gemini/config/hooks.json` for as long as it
runs (it is removed again on exit). The hook sends every tool call to the
bridge, which allows read-only tools and asks the Tab5 for everything else.
If the bridge can't be reached, the hook denies the call. The hook only acts
for `agy` processes started by the bridge; your normal `agy` use is
unaffected.

If a project has its own `.agents/hooks.json`, its rules still apply, and a
deny there wins.

## Security

Anyone who can reach the port and knows the token can run commands through
the agent after you approve them on the Tab5. Keep the token private, prefer
Tailscale over exposing the port on untrusted networks, and don't use
`--yolo` (which skips approvals) unless you trust everything on the network.

## Protocol

JSON messages over one WebSocket at `ws://HOST:PORT/ws`; see the docstring at
the top of `bridge_server.py`.
