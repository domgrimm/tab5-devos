#pragma once

#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the primary template application descriptor.
 *
 * Implements the standard devos_app_descriptor_t interface for a drop-in app.
 */
devos_app_descriptor_t *app_template_get_descriptor(void);

/**
 * @brief Register modular demo apps for testing multi-app scalability (12+ apps).
 *
 * Registers lightweight drop-in apps demonstrating different categories
 * and live telemetry callbacks for the launcher carousel.
 */
void app_template_register_demo_apps(void);

#ifdef __cplusplus
}
#endif
