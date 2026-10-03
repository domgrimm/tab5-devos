#pragma once

/* devos_err: a portability shim for the new Jobs engine family so their
 * headers compile in host tests without pulling in all of ESP-IDF. On the
 * target this *is* esp_err_t (AGENTS.md 5.2: new error-returning APIs use
 * esp_err_t); on the host it is a small int enum with the same meaning.
 *
 * Engines under test on the host (devos_jobs' parser/validator/serializer)
 * must not include lvgl.h or devos_core.h; this header keeps their error
 * contract identical on both sides. */

#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t devos_err_t;
#define DEVOS_OK                  ESP_OK
#define DEVOS_ERR_NO_MEM          ESP_ERR_NO_MEM
#define DEVOS_ERR_INVALID_ARG     ESP_ERR_INVALID_ARG
#define DEVOS_ERR_INVALID_STATE   ESP_ERR_INVALID_STATE
#define DEVOS_ERR_INVALID_SIZE    ESP_ERR_INVALID_SIZE
#define DEVOS_ERR_NOT_FOUND       ESP_ERR_NOT_FOUND
#define DEVOS_ERR_NOT_SUPPORTED   ESP_ERR_NOT_SUPPORTED
#define DEVOS_ERR_TIMEOUT         ESP_ERR_TIMEOUT
#define DEVOS_ERR_FAIL            ESP_FAIL
#else
typedef int devos_err_t;
#define DEVOS_OK                  0
#define DEVOS_ERR_NO_MEM          0x101
#define DEVOS_ERR_INVALID_ARG     0x102
#define DEVOS_ERR_INVALID_STATE   0x103
#define DEVOS_ERR_INVALID_SIZE    0x104
#define DEVOS_ERR_NOT_FOUND       0x105
#define DEVOS_ERR_NOT_SUPPORTED   0x106
#define DEVOS_ERR_TIMEOUT         0x107
#define DEVOS_ERR_FAIL            (-1)
#endif
