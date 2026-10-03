#pragma once

/* devos_secrets: named credential references for Jobs (AGENTS.md 5.4 and the
 * "Jobs-compatible actions and events" invariant).
 *
 * CONTRACT (Phase 0). Implementation lands in Phase 4 with durable storage.
 *
 * A job source stores only a logical reference name, e.g.
 * secret("health-token"); the value is resolved immediately before a
 * credential-capable provider field uses it, copied into bounded transient
 * storage, and wiped after the operation completes or is cancelled. Secret
 * values are never placed in the runtime variable environment, logs, notices,
 * results or exports, and cannot be interpolated into a string. Provider
 * fields declare credential capability in their schema; the resolver refuses
 * to hand a secret to a field that did not.
 *
 * Persistence must be genuinely encrypted before this is called secure:
 * either verified encrypted NVS (partition + keys) or a devos_crypto blob
 * under a provisioned device key. Plain nvs_open() is not proof of
 * encryption, and no hardcoded key is acceptable. Until that is resolved,
 * only non-secret Jobs development proceeds; secret-bearing automation is a
 * gated release item. */

#include "devos_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_SECRETS_MAX       32
#define DEVOS_SECRET_NAME_MAX   40
#define DEVOS_SECRET_LABEL_MAX  48
#define DEVOS_SECRET_VALUE_MAX  512

typedef struct {
    char name[DEVOS_SECRET_NAME_MAX];    /* logical reference */
    char label[DEVOS_SECRET_LABEL_MAX];  /* display only, never the value */
    uint32_t version;                    /* bumped on rotation */
} devos_secret_info_t;

devos_err_t devos_secrets_init(void);
int devos_secrets_count(void);
const devos_secret_info_t *devos_secrets_at(int index);
/* Whether the reference exists (without reading its value). */
bool devos_secrets_has(const char *name);

/* Resolve into `out` (wiped on failure). The caller MUST call
 * devos_secret_wipe() when the provider is done with the copy. Returns the
 * value length, or -1 if the reference is missing/undecryptable. */
int devos_secret_resolve(const char *name, char *out, size_t cap);
void devos_secret_wipe(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
