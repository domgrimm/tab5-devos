#include "devos_storage.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef ESP_PLATFORM
#include <sys/statvfs.h>
#endif

#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#endif

static bool storage_mounted = false;
static uint32_t total_mb = 0;
static uint32_t free_mb = 0;

static const char *WELCOME_MD_CONTENT =
"# Welcome to devOS on M5Stack Tab5!\n\n"
"devOS is a developer-focused mobile cyberdeck firmware for the M5Stack Tab5 with A164 keyboard.\n\n"
"## Markdown Editor Shortcuts\n\n"
"- **`Ctrl + S`**: Save active file\n"
"- **`Ctrl + N`**: Create new note (untitled-N.md)\n"
"- **`Ctrl + O`**: Focus file list\n"
"- **`Ctrl + P`**: Cycle Edit / Split / Preview views\n"
"- **`Sym + L`**: Toggle file sidebar (fullscreen editing)\n"
"- **`Tab`**: Toggle focus between file list and editor\n\n"
"## Global Keyboard Shortcuts\n\n"
"The Tab5 keyboard has no Fn key: hold **`Sym`** for system shortcuts.\n"
"Tap **`Aa`** for caps lock, hold it for Shift.\n\n"
"- **`1` .. `8`**: Quick launch app from Home Screen\n"
"- **`Sym + H`** or **`Esc`**: Global return to Home Screen (Esc goes to the remote shell in Terminal)\n"
"- **`Sym + 1` .. `Sym + 8`**: Instant app switch from anywhere\n"
"- **`Sym + T`**: Toggle Dark Cyberdeck / High-Contrast Light theme\n"
"- **`Sym + -` / `Sym + +`**: Screen brightness\n"
"- **`Sym + F`**: Toggle Focus Mode in AI Agent & Antigravity views\n"
"- **`Sym + L`**: Toggle Left Sidebar (Sessions, Subagents, Bookmarks)\n"
"- **`Sym + R`**: Toggle Right Inspector (Files, Diffs, Artifacts)\n"
"- **`Sym + Up` / `Sym + Down`**: Page Up / Page Down\n"
"- **`Alt + Tab`**: Switch to previous application\n\n"
"## Hardware Quick Reference\n\n"
"| Peripheral | Controller | Bus / Pins | Notes |\n"
"| :--- | :--- | :--- | :--- |\n"
"| SoC | ESP32-P4 | Dual RISC-V @ 400MHz | 32MB PSRAM |\n"
"| Display | 5.0\" 1280x720 | MIPI-DSI | ST7123 |\n"
"| Keyboard | A164 70-Key | Ext.Port1 I2C 0x6D | STM32F030 |\n"
"| Power | NP-F550 | INA226 I2C 0x40 | Telemetry |\n\n"
"## Firmware Sample\n\n"
"```c\n"
"#include \"devos_config.h\"\n\n"
"int main(void) {\n"
"    devos_system_bringup();\n"
"    return 0;\n"
"}\n"
"```\n\n"
"> Distraction-free mobile engineering on the edge.\n\n"
"### Feature Milestones\n\n"
"- [x] Dual-core FreeRTOS architecture\n"
"- [x] 160-column interactive SSH terminal\n"
"- [x] Full Markdown editor with Nimbus Mono 14\n"
"- [ ] OpenCode & OpenChamber remote client\n"
"- [ ] Native Antigravity agent integration\n";

static const char *BOOKMARKS_JSON_CONTENT =
"[\n"
"  {\n"
"    \"alias\": \"Workstation (LAN)\",\n"
"    \"host\": \"10.2.132.54\",\n"
"    \"port\": 22,\n"
"    \"user\": \"root\",\n"
"    \"auth\": \"key\",\n"
"    \"key_path\": \"/sdcard/.ssh/id_ed25519\"\n"
"  },\n"
"  {\n"
"    \"alias\": \"Dev Cluster\",\n"
"    \"host\": \"192.168.1.50\",\n"
"    \"port\": 22,\n"
"    \"user\": \"root\",\n"
"    \"auth\": \"password\"\n"
"  }\n"
"]\n";

static void make_dir_if_missing(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
#ifdef _WIN32
        mkdir(path);
#else
        mkdir(path, 0755);
#endif
    }
}

static void write_file_if_missing(const char *path, const char *content)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(content, f);
            fclose(f);
        }
    }
}

bool devos_storage_bootstrap(const char *mount_point)
{
    if (!mount_point) return false;

    char path_buf[256];

    /* 1. Ensure root mount exists */
    make_dir_if_missing(mount_point);

    /* 2. Scaffold directories */
    snprintf(path_buf, sizeof(path_buf), "%s/.ssh", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/notes", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/plans", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/diffs", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/.devos", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/.devos/logs", mount_point);
    make_dir_if_missing(path_buf);

    /* 3. Scaffold starter template files */
    snprintf(path_buf, sizeof(path_buf), "%s/notes/welcome.md", mount_point);
    write_file_if_missing(path_buf, WELCOME_MD_CONTENT);

    snprintf(path_buf, sizeof(path_buf), "%s/.ssh/bookmarks.json", mount_point);
    write_file_if_missing(path_buf, BOOKMARKS_JSON_CONTENT);

    snprintf(path_buf, sizeof(path_buf), "%s/.ssh/known_hosts", mount_point);
    write_file_if_missing(path_buf, "");

    snprintf(path_buf, sizeof(path_buf), "%s/.devos/version.txt", mount_point);
    write_file_if_missing(path_buf, DEVOS_VERSION_STR "\n");

    snprintf(path_buf, sizeof(path_buf), "%s/.devos/config.json", mount_point);
    write_file_if_missing(path_buf, "{\n  \"theme\": \"dark\"\n}\n");

    return true;
}

bool devos_storage_init(void)
{
#ifdef ESP_PLATFORM
    /* Target ESP32-P4 SDMMC initialization */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* The on-board ESP32-C6 (Wi-Fi, via ESP-Hosted) sits on SDMMC *slot 1*; the
     * microSD card is wired to *slot 0*. SDMMC_HOST_DEFAULT() defaults to slot 1,
     * so mounting the card here reconfigures slot 1 and knocks the C6 off the
     * SDIO bus (sdmmc send_scr -> 0xffffffff, C6 boot loop once Wi-Fi is on).
     * Pin the card to slot 0 with its actual GPIOs so the two coexist. */
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = TAB5_PIN_SD_CLK;
    slot_config.cmd = TAB5_PIN_SD_CMD;
    slot_config.d0  = TAB5_PIN_SD_D0;
    slot_config.d1  = TAB5_PIN_SD_D1;
    slot_config.d2  = TAB5_PIN_SD_D2;
    slot_config.d3  = TAB5_PIN_SD_D3;
    slot_config.width = 4;

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(TAB5_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK) {
        storage_mounted = true;
        total_mb = (uint32_t)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        devos_storage_bootstrap(TAB5_SD_MOUNT_POINT);
        /* Free space is filled in by devos_sysmon's background task: the first
         * f_getfree() on a big card can take seconds, too long to block boot. */
        return true;
    }
    return false;
#else
    /* Simulator Storage Emulation */
    storage_mounted = true;
    devos_storage_bootstrap(TAB5_SD_MOUNT_POINT);
    return true;
#endif
}

bool devos_storage_is_mounted(void)
{
    return storage_mounted;
}

uint32_t devos_storage_get_total_mb(void)
{
    return total_mb;
}

uint32_t devos_storage_get_free_mb(void)
{
    return free_mb;
}

bool devos_storage_refresh_stats(void)
{
    if (!storage_mounted) {
        free_mb = 0;
        return false;
    }
#ifdef ESP_PLATFORM
    /* May scan the FAT on the first call for a large card; call from a
     * background task, never the GUI task. */
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(TAB5_SD_MOUNT_POINT, &total_bytes, &free_bytes) != ESP_OK) {
        return false;
    }
    total_mb = (uint32_t)(total_bytes / (1024 * 1024));
    free_mb = (uint32_t)(free_bytes / (1024 * 1024));
#else
    struct statvfs vfs;
    if (statvfs(TAB5_SD_MOUNT_POINT, &vfs) != 0) {
        return false;
    }
    total_mb = (uint32_t)(((uint64_t)vfs.f_blocks * vfs.f_frsize) / (1024 * 1024));
    free_mb = (uint32_t)(((uint64_t)vfs.f_bavail * vfs.f_frsize) / (1024 * 1024));
#endif
    return true;
}
