#pragma once

/* jobs_providers: registers the first-slice Jobs action providers at boot,
 * after devos_actions is ready and before any definition is validated.
 * Kept in main/ so orchestration can depend on several engines without
 * reversing component dependencies (PLAN.md section 4.1). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register every provider's schemas and runtime handlers (idempotent). */
void jobs_providers_register_all(void);

/* Individual providers (also registered through register_all). */
void jobs_system_register(void);
void jobs_http_register(void);
void jobs_network_register(void);

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
