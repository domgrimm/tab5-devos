# devOS Jobs: source-grounded implementation plan

**Purpose:** An executable engineering handoff for coding agents implementing a persistent, keyboard-first automation system on M5Stack Tab5 devOS, with a GUI builder and a text language over the same execution model.

**Repository:** https://github.com/domgrimm/tab5-devos  
**Inspected revision:** `7e3841a229370d56e96c7201f036a47ef9927637`  
**Revision date/title:** 2026-10-03, `docs: republish the 0.5.3 image with Cricket off by default`  
**Analysis date:** 2026-10-03  
**Firmware version at revision:** 0.5.3.

This document proposes implementation; Jobs does not exist in the inspected revision. Existing behavior is identified explicitly below. Proposed file names, APIs, DSL syntax, limits, and acceptance gates are design decisions, not claims about current code. The source was cloned and inspected directly. Firmware compilation, simulator execution, hardware profiling, and destructive API experiments were not performed for this planning task.

## 1. Agent instructions and outcome

Read repository `AGENTS.md`, `PLAN.md`, this document, and the named integration headers before implementation. If HEAD differs from the revision above, inspect changes to those interfaces first. Preserve existing user changes. Implement one atomic phase at a time, following the phase gates in section 16. Update `PLAN.md` only for work actually implemented and verified.

The finished feature must let a user:

1. Create a disabled job from a template, using only the keyboard.
2. Configure an interval, local daily schedule, manual trigger, boot trigger, or supported event trigger.
3. Add typed actions, variables, conditions, and nonblocking delays.
4. Switch between Builder and Text without losing executable behavior or advanced source.
5. Validate and apply a definition explicitly, run it now, and observe each step.
6. Keep jobs running after leaving the Jobs screen and while the display is off.
7. Inspect bounded history, errors, cancellation, and missed executions.
8. Use named secret references without placing credentials in job source or logs.
9. Disable all Jobs activity through Settings > Apps and recover from a bad definition without a boot loop.

**Recommended architecture:** Three engine components, `devos_actions`, `devos_events`, and `devos_jobs`, plus the LVGL `app_jobs`. Add a small `devos_secrets` component for Jobs credentials. Do not introduce an unrestricted interpreter, embed Node-RED, or implement background execution by opening other apps through intents.

**Release boundaries:** Deliver the manual/interval GUI + DSL + HTTP/ping/notify vertical slice first; expand to reliable MQTT, events, daily schedules, and Docker in later gated increments. Treat those later increments as part of the overall plan, not assumed capabilities in the first release.

## 2. What the source actually provides

All paths below are relative to the repository root. Links pin the relevant source to the inspected revision and remain useful if line numbers move on main.

| Area | Inspected source and symbols | Consequence for Jobs |
| --- | --- | --- |
| Architecture rules | [`AGENTS.md`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/AGENTS.md), sections 1 and 5–6 | Network/crypto run on Core 0; LVGL/input and local SD I/O on Core 1. Preserve at least 120 KB contiguous internal SRAM, place large task buffers in PSRAM, use shared engines, and verify keyboard-only UI. |
| Boot and engine enablement | [`main/main.c`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/main/main.c), `START_ENGINE`, `devos_system_bringup`, `gui_task` | Bringup reads app masks before engines. Jobs needs its own conditional engine startup, descriptor registration, and background lifecycle independent of show/hide. |
| App registry and navigation | [`components/devos_core/devos_core.h`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/components/devos_core/devos_core.h), `devos_app_descriptor_t`, `devos_core_open_with`, `devos_core_take_intent` | Intents are one pending navigation request collected in `show()`. They are not an asynchronous automation RPC system. There is no general typed application publish/subscribe API in this header despite documentation describing an event bus. |
| App switches/recovery | `components/devos_core/devos_apps.c`, `devos_core_app_enabled`, `devos_core_apps_boot_kind`, `devos_core_add_restart_check` | Jobs engine must remain off when its app is off. Safe-start currently turns apps on; Jobs also needs a separate safe-start execution pause so enabling everything cannot immediately run bad automations. |
| HTTP | [`components/devos_http/devos_http.h`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/components/devos_http/devos_http.h), `devos_http_submit/poll/cancel`, `devos_http_resp_free` | Ready for a Jobs async adapter. Response ownership transfers to poll caller and must be released. Transport success can include HTTP 4xx/5xx. |
| HTTP implementation limits | `components/devos_http/devos_http.c`, worker section around lines 849–1058 | Eight shared request slots, one FIFO worker, 12 KB target worker stack. Queued cancellation releases immediately; running cancellation signals abort and slot cleanup happens later. Per-phase timeout is not a whole-operation deadline. Lazy initialization and worker creation need a concurrency/failure audit. |
| Network socket policy | `components/devos_net/devos_net.h`, `devos_net_socket_*`, `devos_net_resolve`, `devos_net_socket_route` | All traffic must reuse the shared routing layer, including VPN routing and `.local` resolution. Never replace this with `esp_http_client` or an independent HTTP stack. |
| Ping | [`components/devos_netdiag/nd_ping.c`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/components/devos_netdiag/nd_ping.c), singleton `P`, `devos_ping_start`, `nd_icmp_*` | Starting a ping stops the existing session and can wait up to roughly two seconds. Calling this API from Jobs would disrupt Network. Extract a context-based one-shot probe sharing the existing ICMP helpers. |
| DNS | `components/devos_netdiag/devos_netdiag.h`, `devos_dns_lookup_start/result`, `nd_dns.c` | Diagnostic API has one shared latest result. DNS automation needs a request-specific API or an explicit busy lease. It is a follow-up action, not a first-slice dependency. |
| Wake-on-LAN | `components/devos_netdiag/nd_wol.c`, `devos_wol_send`, `devos_wol_parse_mac` | WoL already exists in current source, contrary to the earlier app suggestion. Reuse it later; audit shared last-error/last-target state before concurrent Jobs use. |
| MQTT | [`components/devos_mqtt/devos_mqtt.h`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/components/devos_mqtt/devos_mqtt.h), `devos_mqtt_publish`, sequence ring getters | Single configured broker, TCP only, four configured subscriptions, 500-message ring, 4096-byte stored payload cap. `publish()` returning zero only means queued. |
| MQTT delivery | `components/devos_mqtt/devos_mqtt.c`, `send_publish`, `handle_packet`, worker | Existing outgoing PUBACK handling does not provide tracked completion; reconnect drops queued publishes. Do not report confirmed delivery or assume QoS 2: existing publish clamps it to 1. |
| Docker | [`components/devos_docker/devos_docker.c`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/components/devos_docker/devos_docker.c), `worker_loop`, `devos_docker_set_active`, `devos_docker_action` | Worker only runs work when UI active; action storage is a single overwriteable slot; results are a shared `note`. Needs a bounded command queue and operation handles before reliable background automation. |
| Storage | `components/devos_storage/devos_storage.c`, `devos_storage_bootstrap`, `devos_storage_init` | Mount uses `max_files = 5`; automatic scaffolding exists. Jobs adds directories/templates without opening one file per job indefinitely. Existing bootstrap returns success without exposing every creation failure; Jobs must report its own I/O failures. |
| Time and telemetry | `components/devos_sysmon/devos_sysmon.h/.c`, `devos_sysmon_time_source`, `devos_sysmon_set_timezone_index`, private `s_snap` | Wall clock restored from RTC, later SNTP; timezone is global. Snapshot is privately locked and GUI copies it into core telemetry. Add a thread-safe compact snapshot for Jobs rather than reading GUI-owned `devos_telemetry_get()` from a worker. |
| Power | `components/devos_power/devos_power.h/.c`, `devos_power_poll`, `devos_power_sleep_now` | Current sleep is display/backlight state, not CPU deep sleep. Jobs should keep running without faking user activity or waking the screen. Future true suspend needs explicit integration. |
| Editor | [`main/apps/app_editor/app_editor.c`](https://github.com/domgrimm/tab5-devos/blob/7e3841a229370d56e96c7201f036a47ef9927637/main/apps/app_editor/app_editor.c), `ED_EDIT_MAX`, `save_file`, `editor_show`, `editor_hide` | Editable text capped at 48 KB, saves on hide and autosaves, supports `open` intent. No transaction/validation callback for executable jobs. Use a Jobs-owned Text view for first release rather than relying on Editor autosave to activate source. |
| Code viewer | `components/devos_ui/devos_codeview.h/.c` | Fast read-only viewer; retains text pointer. Reuse for traces/source display; it is not an editable code control and has no Jobs breakpoint API. |
| UI primitives | `devos_widgets.h`, `devos_focus.h`, `devos_theme.h`, `devos_icons.h`, `main/apps/app_template/app_template.c` | Reuse standard controls, theme listener, focus behavior, icon infrastructure, and descriptor. No launcher modification or new closed enum entry is necessary. |
| Notices and palette | `devos_toast.h`, `devos_cmdpal.h` | Toast enqueue is task-safe. Prefer a UI bridge so engines remain LVGL-independent. Palette keeps pointers to static descriptors, capacity 48; avoid adding a transient command per job. |
| JSON and crypto | `components/devos_json/devos_json.h`, `components/devos_crypto`, `components/devos_totp/devos_totp.c` | Shared JSON reader is span-based/minimal, not a schema validator or full JSONPath implementation. TOTP provides encrypted vault patterns, not a general unattended secret resolver. |
| Credentials | `AGENTS.md §5.4`, MQTT/Docker NVS loaders, `sdkconfig.defaults` | Project requires encrypted persistence. Use of `nvs_open()` alone does not prove encrypted NVS is configured; source inspection did not establish that guarantee. Verify it or implement encrypted storage before calling Jobs secrets secure. |
| Build/tests | Root `CMakeLists.txt`, `main/CMakeLists.txt`, `tools/*_test.c`, `tools/sim/*` | Simulator manually enumerates sources/includes; target builds use IDF components plus main app source list. Update both. Tests commonly use direct compiler commands and isolated working directories. |

### Corrections to the earlier concept

The conceptual discussion should not be treated as an API specification. There is no multi-broker abstraction, no generic secrets picker, no programmatic Docker command completion, no general events registry, and no reusable Editor transaction bridge in the inspected code. Network already includes WoL and Coder's Toolkit exists. Implement the foundations explicitly; do not hide missing behavior behind UI wording.

## 3. Scope and release contract

### 3.1 First working release

Support one source file per job; manual and interval triggers; `set`, `if/else`, bounded `wait`, typed action calls, output binding, and string interpolation. Actions: `http.request`, `network.ping`, `system.notify`, `system.log`, and read-only system snapshot fields. Include job list, builder, dedicated Text view, validator, Run now/Cancel, live trace, history, templates, disabled imports, last-good revisions, and disabled-app behavior.

The first release must support a real NAS/website check in the background while the user uses Terminal or Editor. Both Builder and Text are part of the first release; GUI support must not be deferred indefinitely after the executor.

### 3.2 Complete planned release

Add local daily/weekdays schedules and boot/Wi-Fi/battery triggers, then tracked MQTT publish and MQTT message triggers, then background Docker inspection/actions. Add bounded `repeat` in text with read-only custom block handling in Builder. Finally add optional DNS/WoL and job calls.

### 3.3 Explicitly deferred

No arbitrary C/Lua/JavaScript, shell execution, infinite loops, external packages, inbound webhooks, distributed workflow engine, full cron grammar, full JSONPath, MQTT TLS/multiple brokers, remote job deployment, or automatic destructive action retries. Home Assistant actions may register later when an HA engine exists. SSH execution is not part of this plan; its security and multi-session lifecycle need a separate design.

## 4. Component boundaries and ownership

### 4.1 Dependency structure

- `devos_events`: platform synchronization only; owns bounded typed event delivery. No UI, Jobs, or app dependency.
- `devos_actions`: immutable action schemas, provider registry, result values, asynchronous operation contract. No Jobs or LVGL dependency.
- `devos_secrets`: named credential storage/resolution; platform crypto/storage dependencies only, no Jobs UI dependency.
- `devos_jobs`: parser, validator, serializer, AST, scheduler, executor, trace/history model and storage coordination. Depends on the above and `devos_json`; engine must compile without LVGL.
- `app_jobs`: LVGL rendering, keyboard, forms, text editing, dialogs, intent handling, notice consumption; depends on Jobs and shared UI.
- Provider adapter registration: place in `main/jobs_providers/` initially so orchestration can depend on several existing engines without reversing component dependencies. Move truly generic providers into their owning component only once that component can depend cleanly on `devos_actions`.

Do not make `devos_jobs` depend on `devos_core.h`: that header pulls LVGL. Boot supplies a compact immutable provider availability mask and UI/sysmon bridges supply typed snapshots. Existing dependency cycles must not be enlarged by teaching HTTP/MQTT about Jobs.

### 4.2 Threads/tasks

| Execution owner | Work | Communication |
| --- | --- | --- |
| Jobs scheduler, Core 0 | Tick deadlines, drain triggers, advance AST frames, poll operation handles, publish snapshots | Bounded command/event/result queues; mutex only for short snapshot copy |
| Existing HTTP worker, Core 0 | HTTP/TLS requests | Existing submit/poll/cancel; guarded init and failure reporting |
| Probe worker, Core 0 | At most one Jobs ICMP probe initially | Request-owned probe handle; independent of Network singleton |
| MQTT/Docker workers, Core 0 | Socket ownership and correlated operations | Provider-owned queues and completion handles |
| Jobs storage worker, Core 1 | Read/write source, generations, history, catalogs; parse/validate candidates if advantageous | PSRAM immutable blobs through queues; never calls LVGL |
| GUI task, Core 1 | User drafts, forms, source entry, render bounded snapshots | Nonblocking Jobs API; consumes queued results and notices |

Keep scheduler responsive while any action waits. Never run `devos_http_request`, a synchronous DNS call, or a blocking ping in the scheduler tick. No task per job. Existing HTTP worker is reused; create scheduler/storage tasks only when Jobs is enabled, and probe task lazily. Use pthread equivalents in the simulator. Core 1 storage worker honors the architectural SD rule while preventing long writes in `lv_timer_handler()`; assign it lower priority than GUI and bound its request batches.

### 4.3 Lifecycle

1. Read app boot mask as today.
2. Initialize action/event registries without starting network engines.
3. Initialize existing services and Jobs-enabled required provider engines once. Supply `provider_available` explicitly; HTTP remains a shared service even if the REST UI is off. Network/MQTT/Docker provider policy follows their app enablement, and disabled providers return unavailable without re-enabling apps.
4. Register static action descriptors independent of UI app `init()`. A schema may remain discoverable while its provider is unavailable.
5. `START_ENGINE("jobs", devos_jobs_init(...))` creates bounded resources, starts storage load, and registers no automatic execution until loading and validation are complete.
6. Register `app_jobs_get_descriptor()` via normal registry. Do not change `app_launcher.c` or closed app enums. Use the dynamic descriptor pattern from Template.
7. Start dispatch only after providers, Jobs source load, and system startup barrier are ready. Emit `system.boot` once per normal successful boot. In safe/reverted boot mode keep automatic execution paused until the user resumes; manual tests remain available.
8. `show()` starts UI polling; `hide()` stops/redacts UI-only resources and preserves draft state. Neither starts/stops the background scheduler.
9. Jobs disabled means no scheduler, storage/probe task, subscriptions, or provider acquisitions solely for Jobs. Getters report off without requiring init.

For normal reboot/shutdown, request scheduler quiescence through a lifecycle callback before restarting. Stop new starts, cancel outstanding operations, flush bounded history if storage is available, then let the existing system action proceed. `devos_core_add_restart_check()` can report an active apply transaction; it must not block every restart indefinitely just because monitoring jobs exist. If the existing lifecycle lacks the required callback, add one small generic prepare-restart/shutdown hook and test it. Do not make safety depend solely on `app_jobs.hide()` because another app may be current.

## 5. Canonical model, persistence, and versioning

### 5.1 One definition, several representations

**Persisted authority:** UTF-8 `.job` source plus a versioned execution manifest recording which revision is active and enabled.  
**Execution authority:** immutable, validated AST for the active revision.  
**Builder authority:** a draft AST projecting that same language subset.  
**Text authority:** a draft source buffer, parsed into a candidate AST before apply.

Builder and Text never maintain independent executable definitions. Validation has no side effects. Apply is transactional. Current runs retain their original AST revision; subsequent runs use the newly applied revision.

Separate job identity from display name. Generate a stable UUID or collision-resistant opaque ID on creation. Bind storage paths and history to ID, not user-provided names. Duplicate produces a new ID and disabled state. Renaming cannot detach history. Revision IDs should derive from validated source bytes, with a monotonic catalog revision for UI synchronization.

### 5.2 Recommended directories

Paths shown are relative to `TAB5_SD_MOUNT_POINT`; the simulator root is `./sim_sdcard`, not `/sdcard`.

| Path | Content |
| --- | --- |
| `jobs/<id>.job` | Active editable/exportable source |
| `jobs/examples/*.job` | Disabled starter examples, never scheduled by discovery |
| `.devos/jobs/catalog.json` | Schema version, job IDs, active revisions, enabled state, bounded scheduling metadata |
| `.devos/jobs/revisions/<id>/<revision>.job` | Active/previous validated source generation |
| `.devos/jobs/drafts/<id>.job` | Optional explicit draft checkpoint, never executable |
| `.devos/jobs/history/<id>.jsonl` | Bounded trace segment with header/schema |
| `.devos/jobs/history/<id>.prev.jsonl` | Previous rotated segment |

Extend storage bootstrap to create missing parent directories and disabled examples without overwriting user source. Runtime may load from known active immutable generation even if an externally edited public `.job` file is invalid; display candidate error and retain last-good source. Invalid new files remain inactive.

### 5.3 Apply transaction

1. Snapshot source/draft and base revision; reject over-limit input before allocation.
2. Parse, validate syntax and types, resolve action schemas, and validate policy. Compile to candidate AST. Missing provider or secret can be represented as an unresolved dependency, but enabling or Run now must fail clearly until satisfied.
3. Compare base revision with active revision. Stale editor gets a conflict; preserve its draft and offer Reload or Save as new job. Never silently overwrite.
4. Write immutable generation to temporary sibling file; check every write/close and flush where supported. Reopen and validate stored bytes/checksum before committing the catalog reference.
5. Commit a new catalog generation using temp/previous files and recovery rules. FAT rename/flush are not a promise of full power-loss atomicity; boot must select a complete valid referenced generation, with previous fallback.
6. Scheduler installs candidate AST through one queued revision-swap command. Acknowledge storage and scheduler revisions separately if swap fails so the UI cannot show an uninstalled revision as active. Do not start a new run during the commit/swap window.
7. Update the public source projection and retain previous generation. If the projection write fails after commit, report it and regenerate it later; authority stays with committed revision. Never switch execution to partial public bytes.
8. Garbage-collect only unreferenced generations; running snapshots hold references. Keep active + previous by default and cap retained bytes.

Enabled state lives in the catalog and defaults false for new/imported/duplicated jobs. Source header excludes enabled so a shared script cannot auto-arm itself. Draft autosave never activates a revision. Allow saving an invalid draft explicitly; never replace last-good executable source with it.

### 5.4 External edits and SD failures

No high-frequency scan. Provide Reload/import and rescan on Jobs show, with a bounded periodic scan optional later. An externally changed `.job` is a candidate needing explicit Apply, even for an already enabled job. Delete/import/rename requests also use the storage queue. Serialize Jobs mutations so they do not race its scanner; generic Editor/File Sharing edits are detected by source hash/base revision rather than assumed to participate in the lock.

On SD removal or I/O failure, loaded jobs can continue from RAM; use bounded RAM history and mark persistence degraded. New durable Apply/Enable reports failure, and enabling should not imply persistence. Stop repeated write attempts with backoff. On reinsertion, reconcile catalog generations before flushing logs. Never recreate an absent card as a successful target directory. Jobs workers keep at most one or two file handles open; account for the global five-file limit alongside Editor, voice recordings, File Sharing and maps. Change the mount limit only after measuring memory impact, not as the first fix.

## 6. Text language v1

Use a small purpose-built parser, not YAML or an unrestricted runtime. The prior pseudocode is replaced by the precise consistent syntax below. Language version is mandatory. Keep extensions additive and reject unsupported major versions with a diagnostic.

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

```text
version 1;
job "Website monitor" {
    trigger every 2m;

    http.request(method: "GET", url: "https://example.com/health",
                 timeout: 10s, max_body: 4096) as response;

    if !response.ok || response.status != 200 {
        system.notify(message: "Website failed: ${response.status}", level: "error");
    }
}
```

`response.ok` means transport response received, not status 2xx. Network timeout/resolve failure produces a typed result with `ok = false`, `status = 0` and a bounded error code. The executor can branch on it. Invalid parameters, missing provider/secret, resource exhaustion, and internal failures are execution errors handled by the default stop policy, not silently treated as a healthy probe.

### 6.1 Grammar outline

```ebnf
file         = "version", integer, ";", "job", string, "{", trigger, [ policy ], { statement }, "}";
policy       = "policy", "(", argument, { ",", argument }, ")", ";";
trigger      = "trigger", ("manual" | "every", duration | "daily", string
               | "weekdays", string | "event", string, [ "(", argument, { ",", argument }, ")" ],
               [ "where", expression ]), ";";
statement    = action | set_stmt | if_stmt | wait_stmt | repeat_stmt;
action       = action_id, "(", [ argument, { ",", argument } ], ")",
               [ "as", identifier ], ";";
argument     = identifier, ":", expression;
set_stmt     = "set", identifier, "=", expression, ";";
if_stmt      = "if", expression, block, [ "else", block ];
wait_stmt    = "wait", duration, ";";
repeat_stmt  = "repeat", integer, "as", identifier, block;
block        = "{", { statement }, "}";
```

Trigger arguments are validated against the event schema; `mqtt.message` requires `topic`, optionally `include_retained: false` and `debounce: 0ms`. They declare the broker subscription independently of the `where` filter. The event-trigger syntax is reserved now and enabled in its implementation phase; unsupported triggers produce explicit diagnostics. No inference of broker subscriptions from arbitrary expressions.

Optional `policy(...)` follows the trigger and records portable execution settings: `timeout: 60s`, `overlap: "skip"` or `"queue_one"`, `cooldown: 0ms`. Schema validation rejects unknown fields and enforces bounds. Global automatic pause and per-device enabled state remain in the catalog; policy belongs in source so exports preserve execution behavior. Cooldown applies to automatic run admission after a start; skipped cooldown triggers increment counters. Run now bypasses cooldown with a visible manual-run label but remains subject to overlap/budgets. Define separate per-action debounce/notification throttling later only if needed.

`repeat` is added only in the advanced-language phase. Initially diagnose it as unsupported rather than accepting syntax that cannot run. Exactly one trigger and one job per file; boot is `trigger event "system.boot";`. Manual Run now bypasses the trigger filter but not enabled-provider/secrets checks, runtime bounds, or overlap policy; it may run a disabled job.

### 6.2 Expressions/types

- Values: null, boolean, signed 64-bit integer, finite number, UTF-8 string, typed duration in milliseconds, bounded object result. Do not confuse duration and number implicitly.
- References: lexical job/run-local variables and named output fields, e.g. `nas.ok`, `response.status`, `event.topic`, `system.battery_percent`.
- Operators: `!`, `&&`, `||`, `==`, `!=`, `<`, `<=`, `>`, `>=`, parentheses; short-circuit booleans. Add arithmetic only if needed after this core is tested. Never implicit string-to-number conversion.
- Builtins: `json_get(body, "temperature")`, a narrowly specified direct/path reader with bounded traversal, and `contains(text, substring)`. Define `json_get` paths as dot-separated object members with optional array indexes if implemented; missing value is null. It is not full JSONPath. Extend `devos_json` rather than writing a second JSON parser, and require complete well-formedness checks for values used by Jobs.
- Interpolation: `${reference}` only in string literals; escape rules and braces are lexer-defined. Reject unresolved variables; fail explicitly on expansion exceeding cap. Null renders as `null`. Secret references cannot be interpolated into log/notify text.
- Credentials: `secret("github-token")` returns a tainted reference, not a general string. Resolve only in provider fields declared credential-capable (e.g. `bearer_token`). No automatic expansion into arbitrary strings.
- Comments: `//` to end of line; quoted JSON-style escapes including `\n`, `\t`, `\"`, `\\` and validated Unicode escapes. Source offsets are byte offsets; UI converts to line/column and UTF-8 character positions.
- Action IDs are dotted registry names. Variables are simple ASCII identifiers. All action argument names are explicit; unknown/duplicate arguments and duplicate output names in a scope are errors.
- Variables use lexical scopes; outputs are defined after action completion. A variable created only inside a branch cannot be accessed after it unless declared outside with `set`. Reassignment must preserve declared inferred type; loop index is read-only. No global mutable state in v1.

### 6.3 Parser/compiler

Implement a lexer with explicit bounds and a recursive-descent/Pratt expression parser with enforced depth, or iterative equivalent. Allocate source and AST arena in PSRAM. Store spans, node IDs, symbol table entries, action schema version, and expression nodes; do not retain pointers into a mutable textarea. Validation is a separate pass so parser tests do not need network engines.

Use a compact AST interpreter with an explicit frame stack rather than recursion during execution. Each frame holds node cursor, branch/loop state, and scope index. Every scheduling tick has a CPU/node budget. Keep source mapping from each executable node to byte span for UI highlight and traces. Parser errors return code, span, line, column and actionable message. A candidate with errors never mutates active state.

### 6.4 Builder round-trip guarantee

For every supported visual node, require `parse(serialize(AST))` to preserve AST semantics and action parameter values. Canonical serializer emits version, fixed indentation, escaped strings, deterministic named arguments and explicit terminators.

**Preserving text matters:** keep original source plus spans. Text edits preserve comments naturally. Builder field edits patch only the node span; insertion/reordering edits the smallest containing block. Show a preview if a change regenerates a block and would remove comments. Provide explicit Format source as the only whole-file canonicalization operation. Tests must cover comments, escaped strings, nested branches and advanced blocks.

For a valid executable advanced node the Builder cannot edit, display an opaque `Custom text` card with summary and Edit text. Retain its source bytes/AST unchanged; surrounding supported nodes can be edited without regenerating the opaque subtree. Unsupported language versions or unknown action syntax are invalid candidates, not executable custom blocks. Do not equate “the GUI does not understand this” with “the validator should accept anything.”

## 7. Action registry and asynchronous provider contract

### 7.1 Descriptor schema

Use static, immutable descriptor metadata for each action. Descriptor fields:

| Field | Meaning |
| --- | --- |
| `id`, `schema_version` | Stable identifier such as `http.request`, versioned parameter/output contract |
| `provider_uid`, category, label, description | Ownership and Builder display; provider UID is not a numeric app ID |
| Parameter descriptors | Name, value type, required/default, min/max/length, enum choices, credential capability, help |
| Output descriptors | Typed result fields, nullability, sensitive/tainted flag, short description |
| Effect metadata | Read-only, network send, state mutation; idempotent/retry-safe declaration and recommended timeout |
| Runtime handlers | Validate provider-specific inputs, start, poll, cancel, release |
| Availability | Provider initialized/configured/connected, with machine-readable reason |

Keep schema available without starting engines or touching network. Registry rejects duplicate IDs, invalid descriptors, overflow, and incompatible version replacement. Freeze registration before Jobs definitions are validated in v1; runtime unload/reload is not required. Builder uses this metadata to create forms, and validator uses the same metadata. Never maintain a separate hand-coded GUI parameter table.

### 7.2 API shape (proposed, adapt names consistently)

```c
/* No lvgl.h or devos_core.h in these engine headers. */
esp_err_t devos_actions_register(const devos_action_descriptor_t *descriptor);
esp_err_t devos_action_start(const char *action_id,
                             const devos_action_args_t *args,
                             const devos_action_context_t *ctx,
                             devos_action_handle_t *out);
esp_err_t devos_action_poll(devos_action_handle_t handle,
                            devos_action_state_t *state,
                            devos_action_result_t *result);
esp_err_t devos_action_cancel(devos_action_handle_t handle);
void devos_action_release(devos_action_handle_t handle);
```

Add a tiny portability error header for host builds, or reuse existing project conventions where appropriate; do not pull all ESP-IDF headers into host parser tests. AGENTS requires `esp_err_t` for new error-returning APIs. Existing integer-return APIs remain unchanged behind adapters. Operation state is an out parameter, not overloaded error return. Cancellation means stop future local execution, not undo an already-sent remote action.

Handles include a slot generation to reject stale references. Define every ownership transition: start copies/owns arguments until complete; poll snapshots state; final result has one explicit transfer/retain contract; release frees exactly once. Do not use unprotected stack pointers or cross-core `volatile` flags as the synchronization protocol. Provider cancellation remains responsible for its worker buffer until that worker acknowledges completion, even if the Jobs run has already moved into cancelling.

Distinguish:

- **Pending/working:** submitted but no final result.
- **Completed:** provider operation completed, typed output available.
- **Failed:** request could not execute, invalid/unavailable/internal/resource error.
- **Cancelled:** confirmed local cancellation or completed after cancellation with outcome marked accordingly.
- **Outcome unknown:** cancellation/timeout after a mutating request may have reached the server.

`result.ok` belongs to the action schema and must have documented meaning. A ping timeout is a completed health-check result with `ok=false`. MQTT connection loss before publish is a failed send. HTTP 503 is a completed HTTP exchange with `status=503`. Failures must not be conflated.

### 7.3 Providers and required engine changes

| Action | Inputs / outputs | Implementation |
| --- | --- | --- |
| `network.ping` | host, timeout; `ok`, `ip`, `latency_ms`, `error_code` | Add `devos_probe_submit/poll/cancel/release` or equivalent context API to netdiag. Refactor shared ICMP mechanics from `nd_ping.c`; use request-owned socket/identifier/deadline. Network UI keeps its own session. No call to singleton `devos_ping_start`. |
| `http.request` | method, URL, timeout, max body, bounded header/body parameters, optional bearer/basic secret; `ok`, status, body, truncated, duration, error code | Wrap existing submit/poll/cancel. Cap body, disallow unbounded request progress pointer, copy result needed by run, free HTTP response. Enforce overall provider deadline in addition to underlying per-phase timeouts. |
| `system.notify` | message, level; `queued` | Queue a small notice to app/UI bridge. UI calls existing toast API. Completion is notice queued, not user acknowledgement. Do not wake screen or store secret-tainted text. |
| `system.log` | bounded message; `recorded` | Append sanitized trace entry in RAM; storage flush separately. Never block action on SD write. |
| `mqtt.publish` | topic, payload, retain, qos 0 or 1, timeout; `sent`, `acknowledged` | Extend MQTT with publish ticket/completion. QoS 0 means bytes sent to socket, not broker/application receipt. QoS 1 completes on matching outgoing PUBACK. Reject QoS 2 rather than silently downgrading. |
| `docker.inspect` | container ID/name, timeout; state, health, ID, updated | Request-specific refresh/inspect through Docker worker, independent of UI selection/cache freshness. Resolve a name deterministically; reject ambiguous matches. |
| `docker.start/stop/restart` | container ID/name, timeout; HTTP status, request accepted, outcome unknown | Replace single `s_act_id/s_act_what` slot with bounded correlated queue. Share implementation with UI calls; preserve UI note as presentation of its own operation. |
| `network.wol` (later) | MAC, optional target; packet sent, target | Refactor per-call output/error from existing engine; normalize MAC using shared parser. No promise target woke. |
| `network.dns` (later) | server/name/type/timeout; answers, rcode | Refactor singleton diagnostic result to per-operation result or context helper; do not change an in-progress UI lookup. |

**HTTP changes:** Guard first-use mutex/worker initialization; check task/pthread creation and queue allocation. Consider explicit initialization during boot/provider readiness rather than racing first submit. Limit Jobs to two outstanding HTTP tickets, preserve headroom for interactive REST, and surface busy/admission failure. A run timeout cannot reclaim a running HTTP slot before worker completion. Audit cancellation during DNS/connect/TLS and redirects; document maximum cancellation latency, and add cancellable bounded resolution if a blocking resolver would defeat run deadlines. Preserve certificate verification by default; explicit insecure per job must be visible. Reject cross-origin forwarding of credential headers on redirects, or disable redirects for credential-bearing Jobs requests until safe forwarding is implemented. Do not copy full credential URLs/headers into trace.

**MQTT changes:** Add monotonically generated publish tickets, packet ID matching, pending acknowledgement table, timeout and reconnect-loss results. A lost session resolves every pending ticket once; do not replay automatically. Preserve the MQTT UI publish API as a wrapper if needed. Add subscription ownership acquisition/release so Jobs does not overwrite the user's four configured subscriptions. Validate combined effective subscriptions against a documented limit and report conflicts. Shared broker remains the sole broker; remove `broker: home` from v1 GUI/text examples. Disabling Jobs releases only Jobs ownership; do not stop a connection held by the UI or another consumer.

**Docker changes:** Separate worker existence, UI polling demand, and background operation demand. `set_active(false)` stops UI periodic stats/logs, not command processing or a Jobs-owned inspection. Queue saturation returns error instead of overwriting. Make requested config/endpoint snapshot immutable per operation so settings changes cannot send to the wrong host. Request-specific completion includes HTTP status and transport error; `204`/`304` semantics remain action-specific. A successful restart API response does not prove the service recovered; the job follows it with an explicit wait and health probe.

### 7.4 Future providers

All future compatible apps register headless actions from an engine/provider registration path, not from their LVGL `init()` or `show()`. The command palette may invoke the same action layer later but first uses static Jobs commands (`Open Jobs`, `New job`, `Pause automatic Jobs`). Do not implement parsing arbitrary palette command strings in this feature.

## 8. Events, triggers, and snapshots

### 8.1 Event envelope

Define a bounded event with stable topic, sequence, monotonic timestamp, optional wall time, typed bounded payload, originating provider and correlation ID. Use by-value primitive fields plus explicitly owned PSRAM payload buffers for larger MQTT messages. Topic/payload schemas are registered metadata so Builder and validator understand event fields. Event registry has no UI dependencies.

Publish is nonblocking and never executes a job inline. ISR producers require a separate ISR-safe API and fixed payloads; v1 producers are task context only. Drop/coalesce under pressure with counters; never allow unbounded allocation because a broker publishes quickly. Limit event-match work per tick. Snapshot subscribers before delivery or serialize subscription mutation so removal cannot cause use-after-free.

Recommended initial topics:

| Topic | Producer hook | Payload / semantics |
| --- | --- | --- |
| `system.boot` | main startup barrier after load/providers ready | boot ID and recovery mode; normal automatic trigger once |
| `network.wifi_connected` / `network.wifi_disconnected` | `devos_net` state transitions, or a bridge using locked Wi-Fi status | State transition, SSID/IP where safe; initial state is not a fabricated transition |
| `system.battery_below` | sysmon snapshot bridge | Valid/present battery, threshold crossing, current percentage; hysteresis before rearm |
| `mqtt.message` | MQTT `store_message` after valid packet processing, outside critical lock | topic, payload, qos, retain, truncation, source sequence; copied before ring overwrite |
| `docker.container_state_changed` (later) | Docker poll/inspect comparison | Stable container ID, previous/current state and freshness; only observed transitions |
| `system.clock_changed` / `system.timezone_changed` | sysmon setter/SNTP bridge | Cause and old/new scheduling inputs; recompute wall-time deadlines |

No background host-up/down or container-stopped event magically exists without a poller; implement polling jobs first and document the observation interval. Local file-save event for job candidates is optional later; do not use it to auto-arm edited scripts.

### 8.2 MQTT triggers

Add MQTT wildcard matcher tests for exact topic, `+`, terminal `#`, empty levels and `$` system-topic rules. Trigger registers an effective broker subscription before it can be enabled. Start with one configured broker and shared connection ownership. Default ignores retained messages at subscription/reconnect, preventing retained control state from firing repeatedly. GUI can explicitly opt in and displays that behavior. Each event gets `truncated`; jobs requiring a complete JSON body fail clearly when only a truncated payload is available. Do not poll the message ring as the primary delivery mechanism: UI clear/ring wrap would lose events. The ring is a UI/history facility; hook provider ingress into events.

### 8.3 Thread-safe system snapshot

Add an engine-facing compact snapshot API in sysmon or a new LVGL-free header. Include battery validity/presence/percentage, Wi-Fi state/SSID/IP, wall-time validity, timezone generation, uptime and monotonic time. Sample hardware once through existing sysmon; Jobs never polls I2C itself. Where Wi-Fi can be obtained through `devos_net_wifi_get_status`, use that thread-safe getter rather than the core GUI telemetry pointer. Avoid adding `devos_actions` dependency to sysmon merely to register a schema; the integration bridge can translate the snapshot to actions/events.

## 9. Scheduler and executor semantics

### 9.1 Scheduler

Use a monotonic clock for `every`, `wait`, cooldown and operation/run deadlines; use wall clock only for daily/weekdays schedules and display. On host use `CLOCK_MONOTONIC`; target use `esp_timer_get_time()` behind an injectable clock interface. Use 64-bit arithmetic, checked duration conversions and explicit wrap handling for existing 32-bit source sequences.

- **Interval:** next due is anchored to the schedule phase, not completion time. Skip past missed periods without flooding; count missed occurrences. First automatic run is after the interval following activation/startup, not immediately. Run now does not reset phase.
- **Daily/weekdays:** interpret HH:MM in device timezone; block while time is invalid. On valid time arrival, compute next future occurrence. No historical catch-up by default.
- **DST:** one occurrence per local calendar date/trigger revision. For nonexistent time, skip that date; for repeated time, run once. Use calendar calculation, not adding 86,400 seconds. Test Sydney transitions and at least one northern-hemisphere timezone. If timezone changes, recompute future deadline without repeating an already claimed occurrence in that local date under the stated policy.
- **Clock jumps:** recompute wall-time jobs after sync/change; never affect interval/delay deadlines. Suppress duplicate local occurrences after backward adjustment. Claim the occurrence before dispatch. Persist bounded claim metadata when durable execution is enabled, but do not promise exactly-once across crash/power loss.
- **Boot/event triggers:** ready barrier precedes boot dispatch; safe/reverted mode suppresses automatic runs. Trigger `where` expression evaluates against immutable event data.
- **Overlaps:** default `skip` if same job already running. Optional `queue_one` retains only the latest pending trigger and reports coalescing. No parallel same-job executions in v1; no restart-current policy initially.
- **Debounce/cooldown:** configurable bounded debounce for events and cooldown for notifications/remediation; do not conflate cooldown with interval. Pending timers use monotonic time.
- **Global overload:** bound active runs and pending triggers; reject/coalesce predictably with visible counters. Fair rotation prevents a high-rate job starving others. Automatic runs cannot indefinitely consume all interactive HTTP/network capacity.
- **Disabled/unavailable:** enabled job with unavailable provider is blocked and shows why; do not silently disable or auto-enable its provider. Validate before dispatch and again before each action.

### 9.2 Run state machine

States: queued, running, waiting action, waiting timer, cancelling, succeeded, failed, cancelled, interrupted. Every transition creates a trace record with run ID, definition revision and node ID. Final states occur exactly once.

Each run stores its immutable definition reference, input/event snapshot, variable arena, explicit frame stack, pending operation handle, elapsed time/deadline, per-run trace ring, cancellation flag and step budget. Evaluation advances until it reaches an async operation, timer, final state or tick budget. `wait` stores a wake deadline; it never sleeps the scheduler task. A nested branch executes only its chosen subtree; unchosen nodes show skipped in debugger.

Default execution error policy is stop with diagnostic. Health probe negative results remain inspectable values. Later optional `on_error` block can be added with schema/language versioning; avoid undocumented continuation. Retry is off by default and allowed only for explicitly retry-safe reads with a bounded count/backoff. Mutation failure/timeout is not automatically retried. User cancellation after request transmission must report outcome unknown when appropriate.

`Disable` stops future automatic dispatch and cancels pending triggers, while a currently running job may finish; show `disabled / finishing`. Separate Cancel current action is explicit. Delete waits for cancellation/release or tombstones identity until retained AST references finish. Pause automatic Jobs blocks new automatic starts without deleting enabled state; Run now remains available. Pause is persisted as a global preference.

### 9.3 Run-to-run data and reusable jobs

V1 state is run-local except summary `last_run/last_result` and scheduling claims. Monitoring escalation counters/hysteresis may later use an explicit bounded state schema; never implicitly persist arbitrary variables. Job calls are a later language extension, with named typed inputs, maximum call depth, cycle validation, shared parent time/step budget, propagated cancellation and run correlation. Until implemented, validator rejects `run` rather than allowing unbounded recursion.

## 10. Limits and memory budget

These are conservative starting limits to be measured, not established capacity claims.

| Resource | Initial cap / policy |
| --- | --- |
| Jobs | 32 loaded; additional files listed as over limit without parsing all into RAM |
| Source | 16 KiB per job; UTF-8 and lexer token length limits |
| AST | 128 executable nodes/job, expression/nesting depth 8 |
| Variables | 32/run; string cap 4096 bytes with shared total arena cap |
| Active runs | 4 globally, 1/job; pending automatic triggers 16 global with coalescing |
| Heavy network operations | 2 Jobs HTTP tickets total, 1 Jobs probe; provider-wide admission guard before additional TLS work |
| HTTP response | 16 KiB default, 64 KiB absolute Jobs maximum; truncation explicit |
| Request bodies/headers | 8 KiB body, 4 KiB headers total, max field lengths; credentials separately bounded |
| MQTT job payload | At most current stored-message limit, explicit truncated flag; 16 queued ingress events initially |
| Run duration | 60 seconds default, 5 minutes maximum including wait; user-visible settings |
| Steps/repeat | 256 executed nodes/run; bounded literal repeat maximum 32 when enabled |
| Trace | 128 entries/run, bounded entry size, overwrite count; no whole response bodies |
| History | Last 50 summaries/job; two bounded disk segments, e.g. 64 KiB each/job plus configurable global cap |
| Registry | 64 actions, 32 event topics, finite subscription slots |

Allocate AST arenas, source, results, snapshots, histories and queues of large structs in PSRAM. Keep static arrays at least 1 KB in `EXT_RAM_BSS_ATTR` when eligible. Keep queue messages small (IDs/owned pointers) and make ownership explicit. FreeRTOS control structures, stacks, crypto-sensitive buffers and DMA retain required internal allocation. Aim scheduler stack about 4–6 KiB and storage/probe stacks about 4 KiB initially, then measure high-water marks; these are estimates, not permission to under-size TLS worker stacks.

Track Jobs allocations in Settings > Apps with the existing `START_ENGINE`/memory-cost hooks, including lazy allocations where possible. At maximum load, measure free internal heap and largest contiguous internal block; acceptance follows AGENTS' 120 KB reserve, not just total free heap. Add admission failure when memory insufficient for another expensive request. Preserve OTA flash size and partition headroom using `idf.py size` and `idf.py size-components`. UI list/Builder should render visible rows rather than constructing hundreds of labels for every loaded AST.

## 11. GUI and text editing

### 11.1 Jobs list

Use 260 px collapsible sidebar where needed, `Sym+L` consistent with AGENTS. Main list shows name, enabled state, trigger summary, last result/time, and running/blocked/error indicator. Search filters display names and action/provider names. Show global automatic execution paused/off and persistence degraded prominently. Home telemetry uses three bounded lines: enabled/total, currently running/blocked, last failure. Never copy large histories just to update tiles.

Keyboard: Up/Down select, Enter opens, N new, R Run now, Space enable/disable when list has focus, H history, T text, Esc returns one level then Home. Destructive delete is a separate dialog. UI letter shortcuts must be suppressed while typing. Global `Sym+Space`, `Sym+I`, `Sym+S`, `Sym+T` remain reserved. Buttons provide touch equivalents; show discoverable footer hints and state-specific `get_shortcuts()`.

### 11.2 Builder

Three regions: trigger card, ordered step tree, inspector. Tab/Aa+Tab rotates regions. Arrows select rows/fields; Enter edits, A adds, D deletes with confirmation where appropriate, explicit Move up/down commands reorder within a block. Do not require drag/drop or a shortcut using Sym punctuation. Branches expand/collapse and show parent path; indentation is bounded by language depth.

Selecting an action opens a schema-generated form. Strings use text fields, booleans toggles, enums dropdowns, numbers/durations bounded controls, secret-capable parameters a secret picker, and expression-enabled fields a Literal/Variable/Expression selector. Required fields and provider availability show before execution. Unknown provider but known schema appears unavailable, not missing entirely. `if` can use structured left value/operator/right value; advanced valid expressions remain a custom expression card with text editing.

Job toolbar: Builder/Text, Validate, Apply, Run now, Cancel, History. Draft dirty state and active revision are distinct. Apply does not automatically enable a new job. Run draft requires validation and a distinct temporary-run action, with no scheduled activation; initial release may omit Run draft and require Apply first. Avoid ambiguous “Save” that silently starts automation.

### 11.3 Text view

Implement within `app_jobs` using shared widget/focus/theme infrastructure and a multiline textarea for the 16 KiB source cap. Reuse or extract generic text-edit helpers only when useful; do not copy all Editor file-browser, preview, and voice code. Provide Ctrl+S Apply, Validate button/shortcut, Find, Undo within a bounded budget, and diagnostics list. Ctrl+S must use the apply transaction, not generic file write.

Errors show line/column and message. Selecting a diagnostic moves cursor and highlights the span if feasible. Unsaved invalid source remains a draft on navigation. Returning to Builder requires parse success; if invalid, show last successful Builder projection with a clear stale label and keep Text authoritative as draft. Never regenerate Builder from an invalid partial parse.

General Editor integration is optional: `Open source in Editor` opens a draft or candidate file, then returning to Jobs requires explicit Reload/Validate/Apply. Existing Editor hide/autosave does not activate anything. Do not add Jobs-specific callbacks to general Editor until there is a reusable transaction use case.

### 11.4 Live run view and history

Show active revision/run ID, elapsed time, trigger, highlighted current node, prior success/failure/skipped nodes and bounded output summary. Node IDs map to source spans, enabling read-only source highlight. If editing has changed the draft, highlight the run's retained revision rather than pretending line numbers refer to the current text. Reuse `devos_codeview` for trace/source text; extend it with a generic highlighted-line API if needed.

Poll compact generation snapshots from an LVGL timer only while visible, roughly 5 Hz for active execution and 1 Hz otherwise; avoid full-screen rebuilds. Theme toggles recolor all cards, borders, badges, syntax, diagnostics and traces. Cancel shows cancelling until provider cleanup completes and warns about uncertain remote mutation only when relevant.

History distinguishes transport errors, negative checks, execution errors, cancelled, interrupted, coalesced/skipped triggers and degraded persistence. Include wall time only when valid plus monotonic duration always. Display summaries, not secret-bearing payloads or entire HTTP headers.

## 12. Credential and execution policy

### 12.1 Secrets service

Add named secrets with opaque ID, label and version. Jobs source stores logical reference names; Builder can resolve/display labels without reading secret bytes. Never expose all secrets to the runtime variable environment. Credential-capable provider fields resolve a reference immediately before use, copy only the necessary value into bounded transient storage, and wipe buffers after provider completion. Existing HTTP submission copies headers; ensure copied secrets are wiped in release/cancel paths, not just the resolver's original buffer.

Verify actual encrypted NVS configuration/keys and partition support. If not available, implement an encrypted credential blob using `devos_crypto` and an appropriate device-key provisioning/storage design. Do not use a hardcoded key, claim plain NVS is encrypted, or reuse TOTP PIN unlocking in a way that silently requires an interactive PIN for every unattended job. Encryption-key lifecycle is a first-class hardware decision that must be resolved before enabling secret-bearing automation. Local simulator secrets are test-only and use restricted permissions; never commit them or treat that storage as target security parity.

Initially use dedicated Jobs secrets; reuse configured MQTT password/Docker token inside their owning providers without exporting them. A general migration of all existing app credentials is a separate project. Missing secret blocks execution with reference name only. Rotation affects future operations; an in-flight operation retains its snapshot. Deletion shows dependent jobs and makes them blocked, never substitutes an empty credential.

### 12.2 Logging and imports

Redact authorization, cookies, URL userinfo and sensitive query values before logs. Prefer structured safe summaries that never receive secret values at all. Taint secret-derived strings so they cannot be passed to log/notify or ordinary persisted state. Authentication-related HTTP response fields may also contain secrets; raw response body is not logged by default. Clear trace buffers and released credential buffers appropriately.

Imported jobs remain disabled and show their effects/providers/secret dependencies. Existing job enablement uses explicit user action. This is local UI policy, not a remote permission prompt per run. Mutating actions are marked visibly in Builder and import review. Dry-run validates and previews proposed actions without sending HTTP/MQTT or changing containers; never fake successful mutation outputs. An optional live read-only test mode may perform declared read-only probes but must label them clearly.

Jobs v1 does not expose a network service for remote creation or triggering. Adding inbound webhooks later requires separate auth, routing, request limits and protection from reboot/retained-event replay.

## 13. Planned file map

File names are suggested implementation boundaries; split only where this improves testability, not to create dozens of forwarding files.

| Path | Work |
| --- | --- |
| `components/devos_actions/devos_actions.h/.c`, `CMakeLists.txt` | Registry, schema/value helpers, operation contract and provider dispatch |
| `components/devos_events/devos_events.h/.c`, `CMakeLists.txt` | Topic schemas, bounded ingress, subscriptions, sequence and drop counters |
| `components/devos_secrets/devos_secrets.h/.c`, `CMakeLists.txt` | Named reference catalog, encrypted persistence, resolve/wipe lifecycle |
| `components/devos_jobs/devos_jobs.h` | Public lifecycle, asynchronous commands, compact snapshot/getter APIs, platform-neutral error types |
| `components/devos_jobs/jobs_model.h/.c` | AST, spans, symbols, typed values, revision lifetime |
| `components/devos_jobs/jobs_parse.c` | Lexer/parser and diagnostics |
| `components/devos_jobs/jobs_validate.c` | Type/schema/policy validation, resource limits |
| `components/devos_jobs/jobs_serialize.c` | Canonical emitter and minimal source-patch utilities |
| `components/devos_jobs/jobs_runtime.c` | Frame interpreter, operation polling, cancel, trace |
| `components/devos_jobs/jobs_schedule.c` | Manual/interval/calendar/event triggers, fake-clock interface |
| `components/devos_jobs/jobs_store.c` | Source/catalog transactions, generation recovery, rotation |
| `components/devos_jobs/jobs_platform.c` | FreeRTOS/pthread queues, clocks, allocators, task creation |
| `components/devos_jobs/CMakeLists.txt` | Target dependencies; no LVGL dependency |
| `main/jobs_providers/jobs_providers.h/.c` | Static provider registration/availability/config bridge |
| `main/jobs_providers/jobs_http.c`, `jobs_network.c`, `jobs_system.c` | First-slice adapters; later MQTT/Docker adapters may be split |
| `main/apps/app_jobs/app_jobs.h/.c` | Descriptor, list, lifecycle, command palette, keyboard routing |
| `main/apps/app_jobs/jobs_builder.c`, `jobs_text.c`, `jobs_history.c`, private `app_jobs_int.h` | UI regions and model projection; start in one file if initially small |
| `components/devos_netdiag/nd_ping.c`, header/CMake | Shared ICMP extraction and request-specific probe API |
| `components/devos_http/devos_http.c` | Initialization/failure/cancellation audit, secret buffer wipe, optional deadline improvements |
| `components/devos_mqtt/devos_mqtt.h/.c` | Ticket completion and subscription/connection ownership, ingress events |
| `components/devos_docker/devos_docker.h/.c` | Background command queue, correlated result, explicit inspect |
| `components/devos_sysmon/devos_sysmon.h/.c` or new compact header | Locked engine snapshot and clock/timezone change bridge |
| `components/devos_storage/devos_storage.c` | Scaffold Jobs paths and disabled templates |
| `components/devos_ui/devos_icons.h/.c` | Jobs vector icon on existing 20×20 grid |
| `components/devos_ui/devos_codeview.h/.c` | Optional generic line highlight support, preserve existing viewers |
| `components/devos_core` lifecycle files | Only generic restart/shutdown prepare hook if needed |
| `main/main.c` | Static registry/provider init, conditional Jobs startup, descriptor, ready barrier and safe-start pause |
| Root `CMakeLists.txt`, `main/CMakeLists.txt` | Simulator source/include lists and target component/app inclusion |
| `main/apps/app_template/README.md`, `app_template.c` | Headless provider/event hook examples and explicit UI-only opt-out |
| `README.md`, `PLAN.md`, `AGENTS.md` | Mandatory implementation and future-app compatibility documentation, section 14 |
| `tools/jobs_*_test.c`, `tools/actions_test.c`, `tools/events_test.c` | Deterministic host suites; exact compiler/CMake commands in file headers |

Proposed public Jobs calls: init/shutdown; list/get job summaries; load/validate/apply candidate; set enabled; pause automatic; run now/cancel; fetch run/history snapshot; get capability metadata. Mutating UI calls enqueue requests and return a request ID plus admission status. Follow-up completion contains request ID and revision/conflict diagnostic. Do not return pointers into mutable worker-owned structs. Add disabled-engine-safe getters.

## 14. Mandatory documentation and future-app compatibility hooks

**User requirement:** Update **`README.md`, `PLAN.md`, and `AGENTS.md`** so future apps include the required hooks for Jobs compatibility. These updates are release deliverables and acceptance gates, not optional follow-up housekeeping. Also update the canonical app template so agents can follow the documented contract without inventing it.

### 14.1 `README.md`

Add Jobs to app list/features and screenshot/usage sections only when functional. Explain GUI/Text shared model, background execution, trigger/action coverage by release, enabled vs draft state, Run now/Cancel, history, credential references, storage paths, and startup recovery. Include two tested v1 examples matching the final parser exactly and how to create them without a desktop computer. Describe effect of screen-off, Jobs disabled in Settings, provider disabled/unavailable, single MQTT broker and transport limits. Avoid documenting future actions as shipped.

Add a short **Building Jobs-compatible apps** subsection linking AGENTS and Template, with checklist: headless provider registration, typed action/event schemas, asynchronous operation lifecycle, availability/disablement, safe credentials, and compatibility tests. Document host test and simulator commands plus real verification URL when available, without hardcoding a nonexistent server.

### 14.2 `PLAN.md`

Add a Jobs architecture section using the next appropriate section number; preserve existing historical numbering. Include component boundaries, canonical definition/AST, action/events contracts, supported triggers/actions, storage model and resource limits. Add phased roadmap checkboxes corresponding to section 16. Mark only tested complete work; keep MQTT/Docker/advanced extensions as pending until verified. Record actual profiling results and any contract/version changes. Fix descriptions that imply a general core event bus already exists if the implementation now provides a separate `devos_events` component.

### 14.3 `AGENTS.md`

Add an architectural invariant titled **Jobs-compatible actions and events**, plus a concrete implementation checklist under modular apps/shared engines. Required contract:

1. Any app/engine with automatable operations **must expose those operations headlessly** through `devos_actions`; Jobs must not open its UI, spoof keys, use a launcher ID, or read its widget state to execute them.
2. Register immutable typed schemas from a boot/provider registration hook, separate from LVGL `init/show/hide`. Clearly specify parameter limits/defaults, output meaning, schema version, effect classification and retry safety.
3. Every long/network operation **must support request-specific handles and start/poll/cancel/release**, with bounded queues and documented ownership. Preserve meaningful distinction between queued, sent, acknowledged, accepted, completed and unknown outcome. No shared “last result” or overwriteable single command slot for Jobs.
4. Provide availability/readiness/configuration state safely when never initialized or disabled. Background demand is separate from UI activity. Off apps must not be silently re-enabled; UI-only apps may declare that they expose no Jobs capabilities.
5. Engines with meaningful state/message transitions **must publish documented typed events** via `devos_events` or supply an integration bridge. Emit outside engine locks, never block provider/ISR paths on Jobs, and use explicit bounded copies rather than pointers into reused rings. Define retained/reconnect/initial-state semantics and drop counters.
6. Reuse `devos_net`, `devos_http`, shared JSON and system snapshots. Preserve Core 0 network/crypto, Core 1 presentation/SD and memory reserve rules. Jobs adds no private networking/parser stacks.
7. Secrets are opaque references or remain in provider config. Credential fields are marked; no secret-bearing source/logs/results, no claim of encrypted NVS without verified configuration. Clean up copied credentials after async completion/cancellation.
8. Add host tests covering schema/validation, operation ownership, overload, cancellation, disabled-provider state and relevant event semantics. Verify that Jobs and the app UI can operate concurrently without disrupting each other.
9. Document new actions/events and exact examples in README/PLAN and update Template if the registration contract changes. Maintain schema compatibility or explicitly migrate definitions.
10. New apps must declare **Jobs capability intent**: actions/events implemented, or an explicit UI-only/non-automatable explanation. “Jobs-compatible” is a tested contract, not a label applied to every app automatically.

Add these to AGENTS' implementation workflow before high-level UI work: design public engine operations; inspect action/event dependencies; implement registration and lifetime tests; verify background use; then connect GUI. Do not impose fictitious event emission on an app with no relevant state transitions.

### 14.4 Template and tests

Extend `main/apps/app_template/README.md` with a minimal headless example action, a state-change event, boot registration snippet and disablement behavior. Keep default Template small: demonstrations must not start network tasks or fabricate useful production results. Show a UI-only alternative. Update `app_template.c` only as necessary to reflect real registration patterns, with supporting provider example in a separate sample file if cleaner.

Add a small compatibility test fixture representing a hypothetical future app: registers action schema without constructing LVGL objects; disabled getter is safe; two simultaneous calls have distinct results; cancel/release is exact; event payload survives provider buffer reuse. This verifies the hooks future agents will copy.

**Documentation gate:** Every release PR touching Jobs must list README/PLAN/AGENTS updates or explain why their existing text already accurately covers that narrow change. The initial Jobs implementation must modify all three files and Template documentation. Do not create only a separate Jobs design document and leave them stale.

## 15. Verification matrix

Use deterministic fake providers and an injectable clock for engine tests. Do not rely on live Internet services for parser/scheduler correctness. Host tests run from fresh temporary working directories because simulator settings persist relative to CWD. New test files include exact build/run instructions consistent with current `tools/*_test.c` style; a focused host CMake target is acceptable if documented.

| Suite | Required cases |
| --- | --- |
| Lexer/parser | Valid examples; comments/UTF-8/escapes; unterminated strings; invalid durations; duplicate trigger; excessive source/tokens/depth; unsupported version; malformed expressions; useful error spans |
| Schema/type validation | Unknown action/argument; missing required; duplicate arg/output; incompatible comparisons; out-of-scope variables; unavailable provider; missing secret; invalid enum/range; secret passed to log; null JSON results |
| Round trip | Canonical AST equality; comments retained on field edit; nested if/else; escaped interpolations; opaque repeat/custom nodes preserved during surrounding edits; moving/deleting blocks preserves syntax |
| Runtime | Async negative result branches; wait doesn't block another run; short-circuit evaluation; global step/run budget; per-job overlap; disable while running; cancel before/after action start; stale handles; delete/revision apply while run retains old AST |
| Scheduling | Monotonic interval despite NTP jump; invalid clock; first activation; missed periods; queue/coalesce; fairness; date/DST gaps/repeats; timezone switch; reboot claims and recovery; safe-start paused |
| Events | Queue saturation/drop counter; subscription removal; producer payload reuse; filtering; initial state/reconnect distinctions; MQTT wildcard/retained/truncated handling; no inline action from emit |
| Storage | Full disk; partial source/catalog writes; invalid checksum/version; stale draft conflict; crash at each commit boundary; previous fallback; SD absence/removal; external edit remains candidate; examples disabled; rotation within caps |
| HTTP adapter | Queue full; transport error vs 503; truncated body; timeout/cancel in queue/running/TLS; response freed once; failed lazy worker start; credential wipe; safe redirects; interactive REST remains usable |
| Network adapter | Concurrent UI ping and Jobs probe; distinct ICMP identifiers; bad host/no route; resolver timeout; cancel socket closure; host ping permission failure diagnostic; VPN route invocation |
| MQTT adapter | Queue accepted vs sent vs PUBACK; packet ID match; ack timeout; disconnect resolves all tickets; no automatic replay; UI/job subscription ownership; Jobs disabled releases only own demand |
| Docker adapter | Two commands never overwrite; UI hidden still processes job; endpoint/config snapshot; queue full; bad ID/name; UI selection unchanged; cancellation unknown outcome; inspect freshness |
| Secrets | Missing/rotated/deleted ref; target encryption configuration; malformed blob; no plaintext exports; redaction and taint; buffer wipes in success/failure/cancel |
| UI | Keyboard create/edit/branch/reorder/apply/enable/run/cancel/history/delete; diagnostics navigation; focus restore; typing doesn't trigger hotkeys; opaque block; two themes; touch equivalents; hidden app leaves execution alive |
| Compatibility/regression | Template fixture; app switches/boot recovery; launcher pagination/telemetry; palette capacity; existing HTTP/Network/MQTT/Docker UI behavior; Editor/File Sharing edits; voice SD contention; OTA/restart |

Fuzz parser/serializer with bounded random inputs under AddressSanitizer/UndefinedBehaviorSanitizer on host. Run sanitizer concurrency/ownership tests as appropriate; TSan may require a dedicated small engine harness rather than full LVGL simulator. Record failures reproducibly. Mutation timeout tests must not contact real containers without an isolated fixture.

Target verification: build and size; boot Jobs on/off; measure heap/largest block and stack high-water marks; run at least a multi-hour mixed workload with Terminal, VPN, HTTP jobs, MQTT ingress and screen off. Test SD unavailable and power interruption on an expendable card. Simulator proves logic/UI, not hardware memory reserve, encrypted storage, routing or SD power-loss durability. Report untested hardware gates explicitly rather than marking them complete.

## 16. Implementation phases and agent-sized tasks

Work sequentially through contract-dependent phases. Separate coding agents can take independent tasks only after common headers, limits and semantics are committed; do not let different agents invent incompatible AST/action/event definitions. Each task ends with a small reviewable change, tests, documentation and an explicit handoff of remaining gates.

### Phase 0: rebase analysis and freeze contracts

**Tasks:** Compare HEAD with inspected revision; read current AGENTS/PLAN; confirm target and host build environment; document source differences. Commit initial Jobs roadmap to PLAN and future-app contract to AGENTS. Define public engine headers, error portability, value types, action/event schemas, AST v1 grammar, storage revision authority, availability policy, and initial limits. Record credential encryption investigation.

**Files:** New component headers, PLAN/AGENTS, Template README; no broad engine rewrite yet.

**Gate:** Parser/action contracts agree on types, ownership and failure semantics; build dependency graph has no new LVGL requirement for engine tests; all future-app hooks are written down. Any unresolved encrypted key provisioning is tracked and blocks credential-bearing release, not unrelated manual non-secret Jobs development.

### Phase 1: model, parser, validator and serializer

**Tasks:** Implement immutable AST/source arenas, lexer/expression grammar, manual/interval triggers, action calls, variables, if/else and wait; register fake schemas for tests; diagnostics and canonical serializer; source spans/minimal patch strategy. Add source/AST/depth/variable limits.

**Gate:** Both NAS/website examples parse; invalid candidates cannot replace active AST; parser/validator/round-trip tests and sanitizers pass. No network needed.

### Phase 2: action/event primitives and first providers

**Tasks:** Implement registry and handles; provider lifecycle/failure/admission tests; HTTP wrapper and initialization/error cleanup audit; request-specific network probe; system log/notice queue; engine-facing sysmon snapshot. Event core can be built now, though event-trigger scheduling ships later.

**Gate:** Fake and real HTTP/probe adapters distinguish completion/result semantics; UI Network ping is not interrupted; response memory and operation slots reclaimed exactly once; disabled/uninitialized getters safe. HTTP queue capacity and init failure behavior tested.

### Phase 3: scheduler and interpreter vertical slice

**Tasks:** Core 0 scheduler, fake-clock host implementation, manual/interval dispatch, explicit execution frames, wait, branch/result binding, overlap policy, budgets, cancellation and generation snapshots. Add global pause and ready barrier. Limit provider acquisition and in-flight heavy work.

**Gate:** Two jobs advance while one waits; transport negative results branch; queue saturation/cancel and revision retain/release tests pass; engine has no UI dependencies. A manual website check can complete through the existing HTTP worker.

### Phase 4: durable storage and recovery

**Tasks:** Core 1 storage worker; directories/templates; immutable revision/catalog commit; history rotation; explicit draft vs apply; hashes/conflicts; external edits as candidates; SD degraded mode; safe-start pause; restart/shutdown quiescence.

**Gate:** Injected failure at each apply boundary recovers to a valid active generation or inactive job with an error, never partial executable source. Examples/imports disabled. Card failure does not block scheduler or crash boot. Storage handles stay within global budget.

### Phase 5: first complete GUI + Text release

**Tasks:** Jobs descriptor, icon, telemetry, palette entries; job list, builder forms from schemas, if tree, dedicated Text editor, diagnostics, Apply/Enable, Run/Cancel, live trace/history, shortcut sheet, theme listeners. Update target/simulator builds and normal boot; Template compatibility fixture.

**Gate:** Keyboard-only end-to-end NAS/website workflow; touch equivalent; both themes; leaving Jobs and screen-off preserve execution; Settings app-off prevents tasks/actions; safe-start/reverted boot automatically paused. **README, PLAN and AGENTS all updated**, including exact shipped examples and future-app hooks. This is the first useful release.

### Phase 6: calendar and system-event triggers

**Tasks:** Device-local daily/weekdays scheduling; time validity and clock/timezone change; boot/Wi-Fi/battery events with schemas; debounce/cooldown and retained pending event policies; GUI trigger forms.

**Gate:** DST/invalid time/jump/timezone tests pass; boot executes only after ready barrier and never during automatic recovery pause; battery invalid/absent does not trigger; event overflow is visible and bounded. Update trigger docs/templates.

### Phase 7: reliable MQTT integration

**Tasks:** Publish ticket/ack lifecycle, connection/subscription ownership, typed ingress events, topic matching/retained behavior; MQTT action/trigger Builder forms and examples; preserve existing UI wrappers.

**Gate:** Queued/sent/acknowledged are visibly correct; disconnect/lost ack never falsely reports delivery; Jobs and UI coexist; four configured user subscriptions are not overwritten; ingress burst remains bounded; no credentials in definitions. Current plain-TCP single-broker limitation documented.

### Phase 8: Docker background operation integration

**Tasks:** Correlated command queue, UI/background demand separation, inspect-by-identity, config snapshot, completion/unknown outcomes, effect metadata and remediation cooldown. Reuse provider implementation in UI.

**Gate:** Jobs operates with Docker screen hidden; commands never overwrite each other; UI stats/log selection stays unchanged; restart followed by HTTP check correctly distinguishes accepted request from service recovery. State-change events optional only with explicit freshness semantics.

### Phase 9: advanced language and secondary actions

**Tasks:** Bounded repeat, opaque Builder nodes, narrow JSON extraction, optional WoL/DNS; typed reusable job calls only after cycle/depth/cancel design; optional generic palette action UI.

**Gate:** Advanced Text can be edited around in Builder without source loss; loops hit step/time caps; repeated network work cannot starve interactive actions; DNS/WoL use shared routing and per-call results. Examples/documentation reflect supported version.

### Phase 10: target hardening and final handoff

**Tasks:** Hardware SRAM/PSRAM/stack/size measurements; mixed-workload soak; SD crash/recovery fixture; verified encrypted credential persistence; secure result/log review; documentation audit.

**Gate:** AGENTS memory reserve met, no boot/task allocation failures or leaks, UI remains responsive, build/host/simulator checks pass, real-hardware results recorded, all limitations accurately named. Do not mark hardware checks passed based on host results.

## 17. Reference acceptance scenarios

### A. GUI-created NAS check

New job → name → Every 5 min → Network/Ping host `nas.local`, timeout 3s, output `nas` → If `nas.ok == false` → Notify “NAS offline” → Validate → Apply → enable. Press Text and verify equivalent source. Run now, see nodes advance, leave Jobs for Terminal, and verify subsequent interval run. No existing Network ping session is cancelled.

### B. Text-created HTTP monitor

Paste the website example in section 6. Validate, Apply, Run now against a local HTTP fixture providing 200/503/timeout/large response endpoints. 503 triggers the condition but is not a transport exception; timeout yields `ok=false/status=0`; truncation is reported. An oversized invalid script remains draft. Active last-good definition keeps running.

### C. MQTT event after Phase 7

```text
version 1;
job "Doorbell notice" {
    trigger event "mqtt.message"(topic: "home/doorbell", include_retained: false);
    system.notify(message: "Doorbell pressed", level: "info");
}
```

The trigger arguments declare effective subscription `home/doorbell` and ignore retained messages. The same fields appear in Builder and portable source. Additional `where` filtering can inspect payload fields without changing the declared subscription. Enabling fails with a precise diagnostic if the combined effective subscription budget is exceeded.

### D. Website remediation after Phase 8

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

The source policy sets a 90-second run timeout and 15-minute automatic-run cooldown. Ensure action deadline ceilings plus wait and overhead fit within that run budget. No automatic restart retry after unknown remote outcome. Docker unavailable blocks with diagnostic. Do not infer healthy container from stale UI cache or from restart API acceptance.

### E. Credential reference

```text
version 1;
job "Authenticated health" {
    trigger manual;
    http.request(method: "GET", url: "https://example.com/private/health",
                 bearer_token: secret("health-token"), timeout: 10s) as check;
    system.log(message: "HTTP status ${check.status}");
}
```

Source/export/logs contain only reference label and safe status. Missing ref blocks; rotation affects subsequent run; cancellation wipes provider copies after worker cleanup. Credentials never forwarded to another origin via redirect.

## 18. Risks and decisions to measure

| Risk | Decision / mitigation | Required evidence |
| --- | --- | --- |
| Shared HTTP FIFO starves UI | Jobs ticket cap/admission, bounded deadline, no per-job TLS worker | REST interaction during concurrent scheduled checks |
| Internal SRAM consumed by new tasks | Lazy tasks, PSRAM arenas, measured stacks, global limits | `idf.py size`, largest-block and high-water-mark measurements |
| Current singleton APIs clobber apps | Request-specific probe/Docker/MQTT operations | Concurrency regression tests with UI active/hidden |
| GUI/text diverge or lose source | Same AST, spans, minimal patches, opaque valid advanced blocks | Round-trip and comment preservation corpus |
| SD power loss corrupts enabled definitions | Immutable generation + referenced catalog + previous fallback | Fault injection at all commit stages; target card interruption |
| Script runs while editing/importing/recovering | Explicit Apply, separate enable, startup automatic pause | External edit/import/safe-start acceptance tests |
| Secret persistence only appears encrypted | Verify NVS provision or implement encryption before release | Target config/key lifecycle and ciphertext inspection |
| Bad clock causes repeated mutation | Monotonic timers, calendar claim, skip missed occurrences | DST/jump/reboot tests; honest at-least/at-most execution limits |
| Event flood or self-trigger feedback | Bounded ingress, cooldown/coalescing, per-tick budget | Broker flood and job-emits-event tests |
| Documentation stale for future agents | README/PLAN/AGENTS + Template gate | PR checklist and fixture demonstrating new hooks |

Open hardware questions must be resolved with measurements: actual heap reserve under VPN + SSH + Jobs; TLS cancellation/resolver upper bound; FAT close/fsync semantics on this IDF build; verified encryption provisioning; concurrent SD worker impact on LVGL/recording; suitable global event/payload caps. These do not prevent a host-tested vertical slice, but they prevent claiming production reliability before validation.

## 19. Final definition of done and handoff format

- [ ] Source version and exact implemented scope recorded.
- [ ] One canonical DSL/AST for GUI and Text, bounded parser and diagnostics.
- [ ] Background execution independent of visible app; asynchronous bounded provider operations.
- [ ] Scheduler, cancel, overlap, time validity and recovery behavior tested.
- [ ] Durable definitions, drafts, conflict detection, last-good recovery and bounded history.
- [ ] Credential references, verified target encryption, redaction and provider-copy cleanup.
- [ ] Existing Network/MQTT/Docker/HTTP/Editor behavior preserved and concurrency tested.
- [ ] Full keyboard/touch access, theme propagation, icon/telemetry/shortcuts, no launcher hardcoding.
- [ ] Settings app disablement and safe-start automatic pause verified.
- [ ] **README.md, PLAN.md and AGENTS.md updated for Jobs and required future-app hooks.**
- [ ] Template registration/event/operation examples and compatibility test fixture updated.
- [ ] Target/simulator build lists updated; relevant host tests and UI checks completed.
- [ ] Hardware memory, soak, routing, encryption and SD recovery gates recorded separately.

Each implementing agent's handoff should list completed phase/task IDs, modified interfaces/files, exact test commands/results, scope/version changes, documentation updates, measured resource impact, known limitations, and the next unblocked task. Include real simulator verification URL when available. Never report planned MQTT acknowledgements, Docker background support, encryption or hardware checks as completed based solely on declarations or mocked results.
