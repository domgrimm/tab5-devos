# Jobs: automation on the Tab5

*The complete guide to devOS Jobs. For the short version see the
[README](README.md#jobs); this file is the reference.*

Jobs runs small automations on a schedule or in response to an event, **independently of the app
you are looking at**. A job is a trigger plus a list of steps; it can ping a host, call an HTTP
API, publish to MQTT, restart a container, wait, branch, loop, and tell you what happened.

The engine runs on **Core 0** (network, crypto) and the UI on **Core 1**, so a job keeps running
while you are in another app or the screen is off. It is off entirely when Jobs is switched off in
**Settings > Apps** — and switching it off is the only thing that stops it.

---

## Contents

1. [The screen](#1-the-screen)
2. [Your first job](#2-your-first-job)
3. [The language](#3-the-language)
4. [Action reference](#4-action-reference)
5. [Events](#5-events)
6. [Credentials](#6-credentials)
7. [Storage, revisions and drafts](#7-storage-revisions-and-drafts)
8. [Limits](#8-limits)
9. [Automation without the UI](#9-automation-without-the-ui)
10. [Troubleshooting](#10-troubleshooting)
11. [Key reference](#11-key-reference)

---

## 1. The screen

Jobs is **dashboard-first**: a 260 px job list on the left and a detail area on the right, with four
tabs, a Problems strip and a state-aware toolbar.

```
┌─ Jobs ──────────────────────────────────────────────────────────────────┐
│  JOBS  (Sym+L hides)        New job             3 on · 0 running · ...  │
│  > running  o off           Enabled | daily 07:30 | rev 17 | last: -   │
│  ! failed   * ok            [Overview] [Builder] [Text] [Runs]          │
│  * New job                  TRIGGER                                     │
│    daily 07:30              Every day at 07:30                          │
│  o waiter                   ...                                         │
│  o quick                    Ready - no problems                         │
│  ...                        [Validate] [Apply] [Enable] [Run now] ...   │
└─────────────────────────────────────────────────────────────────────────┘
```

The list's leading glyph is the job's state at a glance:

| glyph | meaning |
| :-- | :-- |
| `>` | running right now |
| `o` | disabled |
| `!` | enabled, and its last run failed |
| `*` | enabled, and its last run succeeded (or it has not run yet) |

**Tabs.**

| tab | what it is |
| :-- | :-- |
| **Overview** | trigger, next run, last result and the recent runs, read-only |
| **Builder** | schema-driven editing: a trigger card, a step tree, and an inspector built from each action's schema |
| **Text** | the raw source, with the same toolbar |
| **Runs** | the run history, and **Sym+Shift+Y** for the last run's step trace |

Builder and Text edit **one definition**. They cannot diverge: the Builder serialises to the same
source you see in Text, and Text re-parses into the same tree the Builder shows.

**Unsaved work.** An edit that differs from the applied revision is a *draft*: the header shows
`(unsaved)`, the Problems strip says so, and the draft survives leaving the app, switching jobs and
a restart. It only counts as unsaved when the text actually differs — tabbing through a form does
not mark a job dirty.

**Apply is transactional.** **Sym+A** writes the source as a new revision and refuses if the job
changed since you loaded it ("Changed elsewhere"). That is not a dead end: **Sym+Shift+R** (or the
palette's *Reload saved job*) reloads the saved version after asking.

**Dry run is a real run.** **Sym+W** executes the draft through the same scheduler path as *Run
now* — it is not simulated, it just leaves no revision or history behind. Steps with an effect
**outside the device** are listed in a confirmation first (a publish publishes, a restart
restarts). A draft whose steps are all read-only or on-device sinks (`system.log`,
`system.notify`) starts immediately.

---

## 2. Your first job

**Sym+N** offers *Blank*, *Duplicate* and any starters found in `/sdcard/jobs/examples/`. A new job
is **disabled** until you enable it — creating a job never starts running things by itself.

Here is a complete one:

```text
version 1;
job "NAS health check" {
    trigger every 5m;
    network.ping(host: "nas.local", timeout: 3s) as nas;
    if !nas.ok {
        system.notify(message: "NAS offline", level: "warning");
    } else {
        system.log(message: "NAS responded in ${nas.latency_ms} ms");
    }
}
```

1. **Sym+N**, pick *Blank*, and the Builder opens.
2. Set the trigger card to **Every** and its value to `5m`.
3. **Sym+U** to add a step; type `ping` in the filter and add *Ping a host*.
4. In its settings, set `host` to `nas.local` and `timeout` to `3s`, and `Save result as` to `nas`.
5. Add an `if` step (`Sym+U` → *Control: if ... { }*) and fill in `!nas.ok`.
6. Add a *Show a notice* step inside it.
7. **Sym+A** to apply, then **Sym+G** to enable.

An interval job first runs **one interval after it is enabled** — so this one runs 5 minutes later,
then every 5 minutes. **Sym+R** runs it immediately without disturbing the schedule.

---

## 3. The language

One file is `version 1;` followed by exactly one `job`:

```text
version 1;
job "name" {
    trigger ...;
    policy(...);          // optional
    ...steps...
}
```

A job may declare typed inputs after its name (see [Reusable jobs](#reusable-jobs)).

### 3.1 Triggers

A job has exactly **one** trigger.

| trigger | syntax | notes |
| :-- | :-- | :-- |
| manual | `trigger manual;` | only **Run now** |
| interval | `trigger every 5m;` | phase-anchored; missed periods are skipped, not queued |
| daily | `trigger daily "07:30";` | device-local time, one run per local date |
| weekdays | `trigger weekdays "08:00" days "Mon,Wed,Fri";` | `days` is optional, default Mon–Fri |
| event | `trigger event "system.boot";` | fires on a typed event (see [Events](#5-events)) |

Daily and weekly schedules are **DST-safe** and are blocked while the clock is not set (no RTC and
no network time means no wall-clock trigger). A timezone or clock change recomputes the next
occurrence rather than skipping a day.

An **event** trigger can carry arguments and a filter:

```text
trigger event "mqtt.message"(topic: "home/doorbell", include_retained: true, debounce: 2s)
    where event.payload == "pressed";
```

| argument | meaning |
| :-- | :-- |
| `topic` | for `mqtt.message` only: the broker subscription filter (default `#`) |
| `include_retained` | accept retained MQTT messages (default off) |
| `debounce` | ignore further events for this long after one fires (storm protection) |

`where` is an optional expression over the `event.*` fields; the trigger only fires when it is
true. Trigger arguments must be **literals** — a `debounce` that depends on runtime data would be
silently ignored, so the validator refuses it.

### 3.2 Policy

`policy(...)` is optional and controls automatic admission:

| field | meaning |
| :-- | :-- |
| `timeout: 90s` | the run's deadline (default **60 s**) |
| `overlap: "skip"` | what to do when a trigger arrives while the job is still running: `"skip"` (default) or `"queue_one"` (keep just the latest) |
| `cooldown: 15m` | minimum gap between **automatic** starts |

`timeout` and `cooldown` must be **literal durations**. **Run now** bypasses the cooldown.

```text
policy(timeout: 90s, overlap: "skip", cooldown: 15m);
```

### 3.3 Steps

| step | syntax | notes |
| :-- | :-- | :-- |
| action | `network.ping(host: "nas.local") as p;` | any action from the [reference](#4-action-reference) |
| assign | `set up = false;` | binds a variable |
| branch | `if cond { ... } else { ... }` | `else` is optional |
| wait | `wait 2s;` | 1 ms … 5 minutes |
| loop | `repeat 3 as i { ... }` | count 1…32; `i` is a read-only integer |
| call | `run "Check site"(url: "https://a/") as ok;` | see [Reusable jobs](#reusable-jobs) |
| return | `return r.ok;` | yields a value to the caller |

Every action call can bind its result with `as <name>`; the fields are then read as
`name.field` (for example `nas.latency_ms`, `r.status`).

### 3.4 Values and types

`null`, `true` / `false`, integers, numbers, strings, and durations.

A duration is a number with a unit: `250ms`, `2s`, `5m`, `1h`. A duration parameter also accepts a
bare integer, read as **milliseconds** (`timeout: 5000` is 5 s) — the same rule the Builder's
duration fields use.

### 3.5 Expressions

```text
!  &&  ||  ==  !=  <  <=  >  >=
```

Grouping with `( )` works. A `null` is false in a condition. Strings compare with `==`.

```text
if !r.ok || r.status >= 500 { ... }
```

### 3.6 Built-in functions

| function | returns |
| :-- | :-- |
| `contains(haystack, needle)` | whether the string contains the substring |
| `json_get(body, "path")` | one value out of a JSON string, or `null` if the path is missing |
| `secret("name")` | a credential, resolved only for a credential field (see [Credentials](#6-credentials)) |

`json_get` takes a dot-separated path with optional array indexes: `json_get(body, "items[1].name")`.
A missing path is `null`, never an error. An object or array comes back as its own JSON text, so you
can log it or feed it to another `json_get`. It is deliberately narrow, not full JSONPath.

```text
http.request(method: "GET", url: "http://api.example/status") as r;
set state = json_get(r.body, "services.db.state");
if state == "down" { system.notify(message: "DB is down", level: "error"); }
```

### 3.7 Interpolation

Inside a string, `${reference}` is replaced with the value:

```text
system.log(message: "NAS responded in ${nas.latency_ms} ms");
```

A reference is a variable, a `system.*` field or an `event.*` field.

### 3.8 Reading device state

`system.*` is available anywhere in a job:

| group | fields |
| :-- | :-- |
| battery | `battery_percent`, `battery_valid`, `battery_present`, `charging` |
| Wi-Fi | `wifi_connected`, `wifi_ssid`, `local_ip`, `wifi_rssi` |
| Tailscale | `tailscale_online`, `tailscale_ip`, `tailscale_hostname` |
| WireGuard | `wireguard_online`, `wireguard_name`, `wireguard_address` |
| device | `uptime_s`, `time_valid` |
| memory | `psram_free_kb`, `sram_free_kb`, `sram_largest_kb` |
| CPU | `cpu_core0`, `cpu_core1` |

```text
if system.battery_valid && system.battery_percent < 20 && !system.charging {
    system.notify(message: "Battery low", level: "warning");
}
```

### 3.9 Reading the triggering event

Inside an event-triggered job (and in its `where` filter):

| field | meaning |
| :-- | :-- |
| `event.topic` | the topic that fired |
| `event.source` | provider-specific source (for `mqtt.message`, the broker topic) |
| `event.payload` | the payload text (bounded; check `event.truncated`) |
| `event.seq` | a monotonic sequence number |
| `event.truncated` | true when the payload was cut to fit |
| `event.retain` | true for a retained MQTT message |

### 3.10 Reusable jobs

A job can declare typed inputs and return a value, and another job can call it:

```text
version 1;
job "Check site"(url: string) {
    trigger manual;
    http.request(method: "GET", url: url, timeout: 5s) as r;
    return r.ok;
}
```

```text
version 1;
job "Two sites" {
    trigger every 10m;
    run "Check site"(url: "https://a.example/health") as a;
    run "Check site"(url: "https://b.example/health") as b;
    if !a || !b { system.notify(message: "A site is down", level: "error"); }
}
```

Inputs are type-checked at the call. A call runs inline on the caller's frame stack, sharing its
time and step budget, so a called job stops with its caller. Nesting is limited to four deep, and a
call that would re-enter the same job is refused as a cycle. A job with no `return` yields `null`,
which is false in an `if`.

---

## 4. Action reference

Parameters marked **\*** are required. "expression" means the field accepts a variable or a full
expression as well as a literal. `as name` binds the outputs.

### `http.request` — HTTP request

| parameter | type | notes |
| :-- | :-- | :-- |
| `method` | string | `GET` `POST` `PUT` `DELETE` `HEAD` `PATCH` (default `GET`) |
| `url` **\*** | string, expression | up to 1024 bytes |
| `timeout` | duration, expression | 200 ms … 120 s (default 10 s) |
| `max_body` | int, expression | 1 … 65536 bytes of response body kept (default 16384) |
| `bearer_token` | string, **credential** | must be `secret("name")` |
| `body` | string, expression | request body, up to 8192 bytes |
| `headers` | string, expression | extra header lines, one per line: `Name: value` (up to 2048 bytes) |
| `insecure` | bool | `https`: accept any certificate (self-signed LAN services) |

Outputs: `ok`, `status`, `body`, `truncated`, `duration_ms`, `error`.

`ok` means the request completed, not that the status was 2xx — check `status` when it matters. A
redirect is followed (up to five hops). A transport failure still binds `error`, so
`if !r.ok { system.log(message: r.error); }` tells you why.

Webhooks that need their own headers take them here, one per line:

```text
http.request(method: "POST", url: "https://ntfy.sh/tab5",
             headers: "Content-Type: text/plain\nX-Priority: 4",
             body: "Backup finished") as r;
```

`insecure` skips certificate verification for a self-signed LAN service. A credential still
belongs in `bearer_token` as `secret("name")` — a token written into `headers` would sit in the
job source in clear text.

**From the REST app.** You can author a request in **REST** (its form, `{{variables}}` and response
viewer are friendlier than a text field) and turn it into a job with **Sym+J** there, or the
command palette's *REST: Create a job from this request*. It builds the `http.request` call above
from what is on screen — method, URL, headers, body and the certificate check, with the
Content-Type the app would have added — creates the job and opens it in Jobs. Variables are
expanded as they are when sending, and if any were used the toast says so: use `secret("name")`
for credentials instead.

### `network.ping` — Ping a host

| parameter | type | notes |
| :-- | :-- | :-- |
| `host` **\*** | string, expression | up to 253 bytes |
| `timeout` | duration, expression | 200 ms … 30 s |

Outputs: `ok`, `ip`, `latency_ms`, `error_code`.

### `network.wol` — Wake-on-LAN

| parameter | type | notes |
| :-- | :-- | :-- |
| `mac` **\*** | string, expression | `aa:bb:cc:dd:ee:ff` |
| `target` | string, expression | empty = broadcast; a host name, IPv4 address or directed broadcast |

Outputs: `sent`, `target`, `error`.

The target is resolved on a background worker, so the scheduler is never blocked by DNS.

### `network.dns` — DNS lookup

| parameter | type | notes |
| :-- | :-- | :-- |
| `name` **\*** | string, expression | the name to resolve |
| `server` | string, expression | empty = the DHCP server; `ip` or `ip:port` |
| `type` | string | `A` `AAAA` `CNAME` `MX` `TXT` `NS` `PTR` `SRV` `SOA` `ANY` (default `A`) |

Outputs: `ok`, `rcode`, `count`, `first`, `error`.

Each lookup owns its own context, so it never disturbs a lookup in the Network app. Jobs runs one
DNS lookup at a time.

### `system.log` — Log a message

| parameter | type | notes |
| :-- | :-- | :-- |
| `message` **\*** | string, expression | up to 512 bytes |

Output: `recorded`. Appends a line to a bounded in-RAM ring.

### `system.notify` — Show a notice

| parameter | type | notes |
| :-- | :-- | :-- |
| `message` **\*** | string, expression | up to 512 bytes |
| `level` | string | `info` (default) `success` `warning` `error` |

Output: `queued`. Raises a toast **whatever app is on screen**:

| level | toast |
| :-- | :-- |
| `success` | ✓ green, like the "Applied" toast |
| `info` | bell, accent |
| `warning` | amber |
| `error` | red |

`queued` means "handed to the UI", not "the user saw it" — the same contract as `system.log`.

### `mqtt.publish` — MQTT publish

| parameter | type | notes |
| :-- | :-- | :-- |
| `topic` **\*** | string, expression | up to 192 bytes |
| `payload` | string, expression | up to 4096 bytes |
| `retain` | bool | |
| `qos` | int | `0` (default) or `1`; **QoS 2 is refused, not downgraded** |
| `timeout` | duration, expression | 500 ms … 60 s (default 10 s) |

Outputs: `sent`, `acknowledged`, `error`.

The broker is the one configured in the MQTT app (plain TCP, no TLS) and its password never appears
in a job. QoS 1 waits for the PUBACK: `acknowledged` distinguishes "sent" from "the broker took
it". A lost session resolves every unfinished publish once and never replays it.

### `docker.inspect` — Inspect a container

| parameter | type | notes |
| :-- | :-- | :-- |
| `container` **\*** | string, expression | id or name |
| `timeout` | duration, expression | 1 s … 30 s (default 12 s) |

Outputs: `ok`, `status`, `state`, `health`, `id`, `name`, `updated`, `error`.

### `docker.start` / `docker.stop` / `docker.restart`

| parameter | type | notes |
| :-- | :-- | :-- |
| `container` **\*** | string, expression | id or name |
| `timeout` | duration, expression | 1 s … 30 s (default 12 s) |

Outputs: `status`, `accepted`, `outcome_unknown`, `error`.

`accepted` means the daemon took the request (HTTP 204/304), **not** that the service recovered.
Follow a restart with a `wait` and a health check:

```text
version 1;
job "Recover web service" {
    trigger every 5m;
    policy(timeout: 90s, overlap: "skip", cooldown: 15m);
    http.request(method: "GET", url: "http://web.local/health", timeout: 5s) as before;
    if !before.ok || before.status != 200 {
        docker.restart(container: "web", timeout: 15s) as restart;
        wait 10s;
        http.request(method: "GET", url: "http://web.local/health", timeout: 5s) as after;
        if !after.ok || after.status != 200 {
            system.notify(message: "Web recovery failed", level: "error");
        }
    }
}
```

A transport error on a mutation reports `outcome_unknown` and is **never retried automatically** —
the command may or may not have reached the daemon. Docker commands run in the background whether
or not the Docker screen is open, and a job's commands never overwrite each other.

### `proxmox.guest_status` — Read a VM or container

| parameter | type | notes |
| :-- | :-- | :-- |
| `vmid` **\*** | int, expression | the guest's id; its node and type are looked up for you |
| `timeout` | duration, expression | 1 s … 30 s (default 12 s) |

Outputs: `ok`, `status`, `state` (`running` / `stopped` / `paused` / `suspended`), `node`, `name`,
`type` (`qemu` or `lxc`), `cpu` (0…1 of one core), `mem` (bytes), `maxmem` (bytes), `uptime`
(seconds), `error`.

### `proxmox.guest_start` / `guest_stop` / `guest_shutdown` / `guest_reboot`

| parameter | type | notes |
| :-- | :-- | :-- |
| `vmid` **\*** | int, expression | the guest's id |
| `timeout` | duration, expression | 1 s … 30 s (default 15 s) |

Outputs: `status`, `accepted`, `task` (the UPID Proxmox returned), `outcome_unknown`, `error`.

A guest is addressed by **vmid alone**: the engine resolves its node and type from the cluster
resources, so a job never has to know which node a guest lives on. `guest_stop` is a hard stop;
`guest_shutdown` asks the guest to shut down cleanly. `accepted` means Proxmox took the task
(HTTP 200 with a UPID), **not** that the guest reached the state — follow it with a `wait` and a
`guest_status` poll:

```text
version 1;
job "Reboot the web VM" {
    trigger manual;
    proxmox.guest_reboot(vmid: 100, timeout: 15s) as reboot;
    if reboot.accepted {
        wait 30s;
        proxmox.guest_status(vmid: 100) as after;
        system.notify(message: "web VM is ${after.state}", level: "success");
    }
}
```

A vmid that is not in the cluster is a real failure and nothing is sent. A transport error on a
mutation reports `outcome_unknown`. Settings live in the Proxmox app (**C**): the server URL, an API
token id (`user@realm!tokenid`), its secret (kept in NVS) and "accept any certificate" for Proxmox's
self-signed one. Proxmox commands run in the background whether or not the Proxmox screen is open.

---

## 5. Events

| topic | fires when | fields |
| :-- | :-- | :-- |
| `system.boot` | once per normal boot, after the ready barrier | `boot_id:int`, `recovery:boolean` |
| `network.wifi_connected` | Wi-Fi associates | `ssid:string`, `ip:string` |
| `network.wifi_disconnected` | Wi-Fi is lost | `ssid:string` |
| `network.tailscale_connected` | Tailscale comes up | `ip:string`, `hostname:string` |
| `network.tailscale_disconnected` | Tailscale goes down | `hostname:string` |
| `network.wireguard_up` | a WireGuard tunnel comes up | `name:string`, `address:string` |
| `network.wireguard_down` | a WireGuard tunnel goes down | `name:string` |
| `system.battery_below` | a valid, present pack crosses the low threshold | `percent:int`, `charging:boolean` |
| `mqtt.message` | a message arrives on the configured broker | `source:string`, `payload:string`, `retain:boolean`, `qos:int`, `truncated:boolean`, `seq:int` |

The network and battery topics are **transition-only** — they fire on the change, not on every
telemetry tick. An absent or invalid battery never fires `system.battery_below`. `system.boot`
fires once, after the ready barrier, and never during a recovery boot.

An `mqtt.message` trigger declares the broker subscription it needs, so the broker sends only the
topics that jobs actually want:

```text
trigger event "mqtt.message"(topic: "home/doorbell", include_retained: true);
```

Retained messages are ignored unless `include_retained: true`. The trigger's subscription is
deduplicated against the user's own, and released when the job is disabled or deleted.

---

## 6. Credentials

A job **never** stores a password or token. A credential-capable field takes `secret("name")`:

```text
http.request(method: "GET", url: "https://api.example/health",
             bearer_token: secret("health-token")) as r;
```

Values live in an encrypted store sealed with ChaCha20-Poly1305 under a device key. The engine
resolves a credential only for the field that needs it and **wipes its copy as soon as the action
starts**. A credential can never be interpolated into a string or logged.

Add one:

* **Settings > Jobs Secrets** — a reference name and a hidden value; the list shows names and
  versions, and Delete asks twice. The value is never shown again.
* `devos_secrets_set()` from code.
* Drop `/sdcard/.devos/secrets.import` with `name=value` lines (you can upload it from
  **Settings > File Sharing**). It is sealed in, wiped and removed the next time Jobs starts.

**What the encryption actually gives you.** The blob on the card is genuinely encrypted, so a copy
of the SD card alone is ciphertext. The device key lives in plain NVS, which is the weak link until
NVS encryption is enabled. An `nvs_keys` partition is reserved for that; enabling
`CONFIG_NVS_ENCRYPTION` and flashing the keys is the production hardening step and is **not yet
verified on hardware**.

---

## 7. Storage, revisions and drafts

Everything lives on the MicroSD card:

| path | what it is |
| :-- | :-- |
| `/sdcard/jobs/<id>.job` | the job's source, as a readable projection you may edit on a PC |
| `/sdcard/jobs/examples/*.job` | starter examples, offered by **Sym+N**, disabled until enabled |
| `/sdcard/.devos/jobs/catalog.json` | which revision is active and whether the job is enabled |
| `/sdcard/.devos/jobs/revisions/<id>/<rev>.job` | immutable generations, for the Revisions view |
| `/sdcard/.devos/jobs/history/<id>.jsonl` | run history, bounded and rotated |
| `/sdcard/.devos/jobs/drafts/<id>.job` | your unsaved edits |
| `/sdcard/.devos/secrets.enc` | the sealed credential blob |

**Applying writes a new revision**; the previous ones stay. **Sym+Z** lists them, shows one or a
diff against the current draft, and rolls back — a rollback is an ordinary apply of the old source,
so it becomes a new revision rather than rewriting history.

The revision log is authoritative; `jobs/<id>.job` is a projection. If you edit it on a PC, the
change is a *candidate*: it is picked up on the next load only if it parses and validates, and an
invalid external edit leaves the last good revision running rather than breaking the job.

Without a card (or if a write fails) Jobs keeps working in RAM, but nothing survives a reboot and
the Problems strip and the Home tile say so.

---

## 8. Limits

| limit | value |
| :-- | :-- |
| jobs | 32 |
| source per job | 16 KiB |
| AST nodes | 128 |
| nesting depth | 8 |
| variables | 32 |
| string value | 4096 bytes |
| steps per run | 256 |
| `repeat` count | 1 … 32 |
| `wait` | 1 ms … 5 minutes |
| run timeout | 60 s default (policy `timeout`) |
| active runs | 4 (1 per job) |
| `json_get` path | 96 bytes |
| job call depth | 4 |
| job input parameters | 8 |
| trace entries per run | 128 |
| run history | 50 summaries per job, rotated |

The worst-case step count is checked **before** a job is installed, so a definition that could not
fit is refused at validation rather than failing halfway through with side effects already done.

---

## 9. Automation without the UI

Jobs is not a special case: its operations are exposed headlessly so other automation can drive them
without opening the Jobs screen, spoofing its keys or reading its widgets.

* **Actions** are typed schemas in `devos_actions`: id, version, provider, typed parameters with
  required/default/bounds/enum/credential, typed outputs, effect class, retry safety and whether
  the effect stays on-device. A long operation uses a request-specific handle with
  `start`/`poll`/`cancel`/`release`, and distinguishes pending, sent, acknowledged, accepted,
  completed and outcome-unknown.
* **Events** are bounded, typed, and published outside engine locks with copied payloads.
* **Availability** is reported safely even when an engine was never initialised or its app is
  switched off — a switched-off provider says so instead of racing an uninitialised engine.

An app that adds an automatable operation registers a schema from a boot hook (not from its LVGL
`init()`) and states its Jobs capability in its own README. See
[`main/apps/app_template/README.md`](main/apps/app_template/README.md).

---

## 10. Troubleshooting

**"Changed elsewhere" when applying.** The job moved on since you loaded it. **Sym+Shift+R**
reloads the saved version (it asks first, because that discards your draft), or use the palette's
*Reload saved job*.

**A job never runs.** Check, in order: is it enabled (`*`/`!` not `o`)? Is automatic execution
paused (the toolbar's Pause, or a recovery boot — the header says `paused`)? For a wall-clock
trigger, is the clock set (`system.time_valid`)? For an event trigger, the Problems strip warns
when the topic no longer has a registered schema, which means it can never fire.

**The Problems strip shows a Problem.** The first validation diagnostic, in full. The Builder's
Problems line is the primary feedback channel — the toast only says there is one, because a
diagnostic is longer than a toast.

**"outcome unknown" on a Docker restart.** The command was sent but the daemon never confirmed it.
It may or may not have been applied; that is why it is never retried automatically. Follow it with
a `wait` and a health check.

**`level` (or another choice) reverted.** Choice fields are committed from their dropdown; pick a
value and it is written on the next commit (defocus, **Enter**, or **Sym+A**).

**A step is "Advanced step — edit it in Text".** The Builder cannot render that node (a `repeat`
body, or an action whose schema the Builder does not have). Editing the text is the escape hatch;
the step still round-trips.

**The engine is off.** Jobs is switched off in **Settings > Apps**, or the engine failed to start.
The Home tile and **Sym+I** say which.

---

## 11. Key reference

**Job list**

| key | action |
| :-- | :-- |
| `Up` / `Down` | pick a job |
| `Enter` / `Tab` | open it (focus the view) |
| `Space` | enable / disable |
| `B` `M` `O` `Y` | Builder / Text / Overview / Runs |
| `R` `N` `D` | Run now / New / Delete (asks first) |
| `E` `Z` | Job settings / Revisions |
| `Sym+L` | show / hide the job list (hiding it reclaims the 260 px) |

**Anywhere**

| key | action |
| :-- | :-- |
| `Sym+C` | validate |
| `Sym+A` | apply (writes the unsaved changes) |
| `Sym+G` | enable / disable |
| `Sym+R` | run now |
| `Sym+X` | cancel a running job |
| `Sym+E` | job settings (name, timeout, overlap, cooldown) |
| `Sym+W` | dry-run the draft (really executes — asks first) |
| `Sym+Z` | revisions (view, diff, roll back) |
| `Sym+Shift+Y` | last run's step trace |
| `Sym+Shift+R` | reload the saved version (discards changes) |
| `Sym+B` / `Sym+M` / `Sym+O` / `Sym+Y` | Builder / Text / Overview / Runs |
| `Esc` | back out one level: field, then panel, then the job list, then the Home Screen |

**Builder**

| key | action |
| :-- | :-- |
| `Up` / `Down` | pick a step |
| `Sym+U` / `Del` | add a step / delete the selected step |
| `Sym+K` / `Sym+J` | move the step up / down (or drag it) |
| `[` / `]` | outdent / indent (into an `if` or `repeat` body) |
| `Enter` | commit the field you are editing |
| `Tab` / `Aa+Tab` | move between regions (never trapped) |

`Sym+S` opens the shortcut sheet from anywhere, and `Sym+Space` the command palette — which offers
*Jobs: New job*, *Jobs: Pause/Resume automatic* and *Jobs: Reload saved job*.
