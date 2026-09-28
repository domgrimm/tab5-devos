#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * devOS Hardware & Architecture Configuration for M5Stack Tab5
 * ========================================================================= */

#define DEVOS_VERSION_MAJOR         0
#define DEVOS_VERSION_MINOR         4
#define DEVOS_VERSION_PATCH         0
#define DEVOS_VERSION_STR           "devOS v0.4.0"
#define DEVOS_BUILD_CODENAME        "Cyberdeck Alpha"

/* --- Display & Screen Geometry --- */
#define DEVOS_SCREEN_WIDTH          1280
#define DEVOS_SCREEN_HEIGHT         720
#define DEVOS_TOP_BAR_HEIGHT        38
#define DEVOS_BOTTOM_BAR_HEIGHT     32
#define DEVOS_CONTENT_HEIGHT        (DEVOS_SCREEN_HEIGHT - DEVOS_TOP_BAR_HEIGHT)

/* --- Side panel widths (editor file list, terminal connections) --- */
#define DEVOS_PANE_LEFT_WIDTH       260
#define DEVOS_PANE_RIGHT_WIDTH      300

/* --- Terminal Geometry --- */
#define DEVOS_TERM_FONT_WIDTH       8
#define DEVOS_TERM_FONT_HEIGHT      16
#define DEVOS_TERM_COLS_EXPANDED    160     /* Full width: 1280 / 8 */
#define DEVOS_TERM_COLS_COLLAPSED   128     /* Panel open: 1020 / 8 */
#define DEVOS_TERM_ROWS             45      /* 720 / 16 */
#define DEVOS_TERM_MAX_SCROLLBACK   10000   /* Lines allocated in PSRAM */

/* --- Hardware Pinouts & Interfaces --- */
#ifdef ESP_PLATFORM
    /* Main I2C Bus for Ext.Port1 (Tab5 Keyboard A164) */
    #define TAB5_I2C_PORT           I2C_NUM_0
    #define TAB5_PIN_I2C_SDA        0
    #define TAB5_PIN_I2C_SCL        1
    #define TAB5_PIN_KBD_INT        50
    #define TAB5_KBD_I2C_ADDR       0x6D

    /* Secondary I2C Bus (Sensors & Power) */
    #define TAB5_SENSORS_I2C_PORT   I2C_NUM_1
    #define TAB5_INA226_ADDR        0x41   /* battery monitor (M5Unified) */
    #define TAB5_RTC_ADDR           0x32
    #define TAB5_GT911_ADDR         0x5D
    #define TAB5_GT911_ADDR_ALT     0x14

    /* MicroSD SDMMC Pins */
    #define TAB5_PIN_SD_CLK         43
    #define TAB5_PIN_SD_CMD         44
    #define TAB5_PIN_SD_D0          39
    #define TAB5_PIN_SD_D1          40
    #define TAB5_PIN_SD_D2          41
    #define TAB5_PIN_SD_D3          42
    #define TAB5_SD_MOUNT_POINT     "/sdcard"

    /* Dual-Core FreeRTOS Task Pinning */
    #define DEVOS_CORE_NET_CRYPTO   0   /* Wi-Fi, lwIP, MicroLink, mbedTLS, SSH */
    #define DEVOS_CORE_UI_INPUT     1   /* LVGL v9, GT911 touch, A164 keyboard, SD I/O */
#else
    #define TAB5_SD_MOUNT_POINT     "./sim_sdcard"
    #define DEVOS_CORE_NET_CRYPTO   0
    #define DEVOS_CORE_UI_INPUT     0
#endif

/* --- Networking Defaults --- */
#define DEVOS_SIM_VNC_PORT          6080

/* --- Battery --- */
/* Stock Tab5 NP-F550-style 2S pack: 7.4 V x 2000 mAh. Used only for the
 * runtime estimate on the home screen. */
#define DEVOS_BATTERY_CAPACITY_MWH  14800

/* --- Applications Enumeration --- */
/* The number is the Sym+<digit> shortcut (Sym+H is Home). */
typedef enum {
    DEVOS_APP_LAUNCHER = 0,     /* Home Screen / App Launcher Dashboard */
    DEVOS_APP_TERMINAL = 1,     /* Sym+1 Terminal/SSH: Multi-session ANSI PTY shell */
    DEVOS_APP_EDITOR = 2,       /* Sym+2 Editor: SD card Markdown & text editor */
    DEVOS_APP_TAILSCALE = 3,    /* Sym+3 Tailscale: Mesh network manager */
    DEVOS_APP_WIREGUARD = 4,    /* Sym+4 WireGuard: tunnel from a wg-quick config */
    DEVOS_APP_MQTT = 5,         /* Sym+5 MQTT: broker monitor & publisher */
    DEVOS_APP_SETTINGS = 6,     /* Sym+6 Settings: Wi-Fi, Display, Power, NVS */
    DEVOS_APP_COUNT,
    DEVOS_APP_NONE = 255        /* no app shown yet */
} devos_app_id_t;

#ifdef __cplusplus
}
#endif
