#pragma once

/* devos_cmdpal: the command palette. Sym + Space from any screen opens a
 * search box over it: type part of a name (devos_match.h), Up / Down (or
 * Tab) pick, Enter runs, Esc or Sym + Space closes. A tap runs a row; a tap
 * outside the box closes it.
 *
 * It lists every app that is switched on, the system commands (theme,
 * info panel, screen off, restart, shutdown) and whatever else was added with
 * devos_cmdpal_add(). Apps add theirs in init(), so a switched-off app's
 * commands are gone with it.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct devos_command {
    const char *title;          /* "Open the scratchpad" */
    const char *keywords;       /* more words it is found by: "notes journal" (NULL = none) */
    const char *hint;           /* right-hand column: where it goes or its key ("Editor", "Sym+T") */
    const char *icon;           /* an LV_SYMBOL_* (NULL = none) */
    /* A title that follows the state ("Switch to the light theme"); NULL = title. */
    const char *(*label)(void *ud);
    /* Ask first: the question shown on Enter, which then runs it on a second
     * Enter (NULL, or a NULL answer = run at once). */
    const char *(*confirm)(void *ud);
    void (*run)(void *ud);
    void *ud;
} devos_command_t;

/* Hook Sym + Space (after the top bar exists) and add the system commands. */
void devos_cmdpal_init(void);
/* Kept by pointer: pass static storage. Up to 48. */
void devos_cmdpal_add(const devos_command_t *cmd);
void devos_cmdpal_open(void);
/* Open with `query` in the box (NULL = empty). run_first runs the best match
 * at once - the Sym+R / Sym+Q shortcuts use it to land on restart / shutdown
 * and raise their confirmation. */
void devos_cmdpal_open_with(const char *query, bool run_first);
void devos_cmdpal_close(void);
bool devos_cmdpal_is_open(void);

#ifdef __cplusplus
}
#endif
