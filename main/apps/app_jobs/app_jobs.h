#pragma once

#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the Jobs application descriptor.
 *
 * A keyboard-first automation app: a job list, a Text editor over the Jobs
 * language, Validate / Apply / Enable / Run now / Cancel, diagnostics and run
 * history. It drives the devos_jobs engine (which keeps running while the app
 * is hidden or the screen is off) and never touches storage directly.
 */
devos_app_descriptor_t *app_jobs_get_descriptor(void);

#ifdef __cplusplus
}
#endif
