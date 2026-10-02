#pragma once

#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the Coder's Toolkit application descriptor.
 *
 * Offline developer helpers - Base64 / Hex encode & decode, SHA & HMAC
 * digests, UUID v4, JWT inspection and Unix-time conversion - built entirely
 * on the shared engines (devos_crypto, devos_json) so nothing is forked.
 */
devos_app_descriptor_t *app_coder_get_descriptor(void);

#ifdef __cplusplus
}
#endif
