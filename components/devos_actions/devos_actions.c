/* devos_actions: the schema registry and the operation runtime
 * (see devos_actions.h). Pure logic - no LVGL, no network - so the Jobs
 * validator and the scheduler's provider contract are unit-tested on the host
 * with fake providers registered through the same API real providers use. */
#include "devos_actions.h"
#include <stdio.h>
#include <string.h>

static const devos_action_descriptor_t *s_actions[DEVOS_ACTIONS_MAX];
static int s_count;

const char *devos_val_type_name(devos_val_type_t t)
{
    switch (t) {
    case DEVOS_VAL_NULL:     return "null";
    case DEVOS_VAL_BOOL:     return "boolean";
    case DEVOS_VAL_INT:      return "integer";
    case DEVOS_VAL_NUM:      return "number";
    case DEVOS_VAL_STR:      return "string";
    case DEVOS_VAL_DURATION: return "duration";
    default:                 return "?";
    }
}

/* A descriptor is usable only if it names an action and every parameter has a
 * name and a valid type. Duplicate ids are refused (a re-register replaces). */
static bool descriptor_ok(const devos_action_descriptor_t *d)
{
    if (!d || !d->id || !d->id[0]) return false;
    if (d->param_count < 0 || d->out_count < 0) return false;
    for (int i = 0; i < d->param_count; i++) {
        if (!d->params[i].name || !d->params[i].name[0]) return false;
        if (d->params[i].type > DEVOS_VAL_DURATION) return false;
    }
    for (int i = 0; i < d->out_count; i++) {
        if (!d->outs[i].name || !d->outs[i].name[0]) return false;
        if (d->outs[i].type > DEVOS_VAL_DURATION) return false;
    }
    return true;
}

devos_err_t devos_actions_register(const devos_action_descriptor_t *d)
{
    if (!descriptor_ok(d)) return DEVOS_ERR_INVALID_ARG;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_actions[i]->id, d->id) == 0) {
            /* a provider may re-register the same contract; a different
             * schema version replaces it (the caller controls the timing) */
            s_actions[i] = d;
            return DEVOS_OK;
        }
    }
    if (s_count >= DEVOS_ACTIONS_MAX) return DEVOS_ERR_NO_MEM;
    s_actions[s_count++] = d;
    return DEVOS_OK;
}

const devos_action_descriptor_t *devos_actions_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_actions[i]->id, id) == 0) return s_actions[i];
    }
    return NULL;
}

int devos_actions_count(void) { return s_count; }

const devos_action_descriptor_t *devos_actions_at(int index)
{
    return index >= 0 && index < s_count ? s_actions[index] : NULL;
}

void devos_actions_reset(void)
{
    s_count = 0;
    memset(s_actions, 0, sizeof(s_actions));
}

/* ------------------------------------------------------------ runtime */
typedef struct {
    bool used;
    uint32_t gen;
    const devos_action_descriptor_t *d;
    void *op;
} slot_t;

static slot_t s_slots[DEVOS_ACTION_OPS_MAX];
static uint32_t s_gen;

static slot_t *slot_of(devos_action_handle_t h)
{
    if (h.slot == 0 || h.slot > DEVOS_ACTION_OPS_MAX) return NULL;
    slot_t *s = &s_slots[h.slot - 1];
    if (!s->used || s->gen != h.gen) return NULL;
    return s;
}

bool devos_actions_available(const char *id, char *reason, size_t cap)
{
    if (reason && cap) reason[0] = '\0';
    const devos_action_descriptor_t *d = devos_actions_find(id);
    if (!d) { if (reason && cap) snprintf(reason, cap, "unknown action"); return false; }
    if (!d->ops || !d->ops->start) { if (reason && cap) snprintf(reason, cap, "no provider"); return false; }
    if (d->ops->available) return d->ops->available(reason, cap);
    return true;
}

int devos_actions_outstanding(void)
{
    int n = 0;
    for (int i = 0; i < DEVOS_ACTION_OPS_MAX; i++) n += s_slots[i].used;
    return n;
}

devos_err_t devos_action_start(const char *action_id, const devos_action_args_t *args,
                               const devos_action_context_t *ctx, devos_action_handle_t *out)
{
    if (!out) return DEVOS_ERR_INVALID_ARG;
    *out = DEVOS_ACTION_HANDLE_NONE;
    const devos_action_descriptor_t *d = devos_actions_find(action_id);
    if (!d || !d->ops || !d->ops->start) return DEVOS_ERR_INVALID_ARG;
    char why[64];
    if (!devos_actions_available(action_id, why, sizeof(why))) return DEVOS_ERR_INVALID_STATE;

    int idx = -1;
    for (int i = 0; i < DEVOS_ACTION_OPS_MAX; i++) {
        if (!s_slots[i].used) { idx = i; break; }
    }
    if (idx < 0) return DEVOS_ERR_NO_MEM;

    void *op = NULL;
    devos_err_t rc = d->ops->start(args, ctx, &op);
    if (rc != DEVOS_OK) return rc;

    slot_t *s = &s_slots[idx];
    s->used = true;
    s->gen = ++s_gen;
    if (s->gen == 0) s->gen = ++s_gen;      /* never hand out generation 0 */
    s->d = d;
    s->op = op;
    out->slot = (uint32_t)(idx + 1);
    out->gen = s->gen;
    return DEVOS_OK;
}

devos_err_t devos_action_poll(devos_action_handle_t handle, devos_action_state_t *state,
                              devos_action_result_t *result)
{
    slot_t *s = slot_of(handle);
    if (!s || !s->d->ops->poll) return DEVOS_ERR_NOT_FOUND;
    return s->d->ops->poll(s->op, state, result);
}

devos_err_t devos_action_cancel(devos_action_handle_t handle)
{
    slot_t *s = slot_of(handle);
    if (!s || !s->d->ops->cancel) return DEVOS_ERR_NOT_FOUND;
    return s->d->ops->cancel(s->op);
}

void devos_action_release(devos_action_handle_t handle)
{
    slot_t *s = slot_of(handle);
    if (!s) return;                          /* already released / stale: no-op */
    if (s->d->ops->release) s->d->ops->release(s->op);
    s->used = false;
    s->op = NULL;
    s->gen = ++s_gen;                        /* invalidate the handle */
    if (s->gen == 0) s->gen = ++s_gen;
}
