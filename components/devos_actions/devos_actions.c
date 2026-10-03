/* devos_actions: the schema registry (see devos_actions.h). Pure logic - no
 * LVGL, no network - so the Jobs validator is unit-tested on the host with
 * fake schemas registered through the same API providers use. */
#include "devos_actions.h"
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

void devos_actions_reset(void) { s_count = 0; }
