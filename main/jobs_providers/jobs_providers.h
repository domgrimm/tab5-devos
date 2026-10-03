#pragma once

/* jobs_providers: registers the first-slice Jobs action providers at boot,
 * after devos_actions is ready and before any definition is validated.
 * Kept in main/ so orchestration can depend on several engines without
 * reversing component dependencies (PLAN.md section 4.1). */

#include <stdbool.h>
#include <stddef.h>
#include "devos_jobs.h"   /* devos_jobs_system_t for the event bridge */

#ifdef __cplusplus
extern "C" {
#endif

/* Register every provider's schemas and runtime handlers (idempotent). */
void jobs_providers_register_all(void);

/* Individual providers (also registered through register_all). */
void jobs_system_register(void);
void jobs_http_register(void);
void jobs_network_register(void);
void jobs_mqtt_register(void);
void jobs_docker_register(void);
void jobs_events_register(void);

/* Optional availability gate for the Docker provider (e.g. the Docker app is
 * switched off in Settings > Apps). Jobs never re-enables an app; a false
 * result makes every docker.* action unavailable with `reason`. NULL clears
 * the gate. Not needed by host tests, so it lives in the provider. */
void jobs_docker_set_gate(bool (*gate)(char *reason, size_t cap));

/* ---- system-event producers (Phase 6) -----------------------------------
 * Called from main's boot path / 1 Hz loop, never from an engine. The bridge
 * publishes typed events (system.boot, Wi-Fi connect/disconnect, battery
 * below) through devos_events; a system-event job's trigger consumes them. */
void jobs_events_publish_boot(bool recovery);
void jobs_events_poll(const devos_jobs_system_t *s);

/* ---- system.log / system.notify sinks (consumed by the Jobs UI bridge) ----
 * `system.log` appends a sanitized line; `system.notify` queues a notice for
 * the UI to toast. Both are bounded and safe to call when the UI is absent. */
#define JOBS_SYSTEM_LOG_MAX     32
#define JOBS_SYSTEM_LOG_LINE    128
#define JOBS_SYSTEM_NOTICE_MAX  16
#define JOBS_SYSTEM_NOTICE_LEN  160

/* Copy up to `max` most-recent log lines into out (each JOBS_SYSTEM_LOG_LINE
 * bytes), newest last. Returns the number copied. */
int jobs_system_log_tail(char *out, int max);
int jobs_system_log_count(void);
/* Pop the oldest queued notice. level: 0 info, 1 warning, 2 error. */
bool jobs_system_take_notice(char *msg, size_t cap, int *level);

#ifdef __cplusplus
}
#endif
