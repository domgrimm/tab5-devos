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
"## Editor\n\n"
"The file list on the left browses the whole SD card: **Enter** opens, **Backspace** goes up a folder, "
"**N** new file, **F** new folder, **R** rename, **D** delete, **H** hidden files. **Esc** switches "
"between the list and the editor.\n\n"
"- **`Ctrl + S`**: Save (it also saves by itself after 30 s idle and when you leave)\n"
"- **`Ctrl + N`**: New file\n"
"- **`Ctrl + P`**: Cycle Edit / Split / Preview\n"
"- **`Ctrl + F`**, **`Enter`**: Find, next match\n"
"- **`Ctrl + Z`**: Undo\n"
"- **`Ctrl + X / C / V`**: Cut / copy / paste (the selection, or the whole line)\n"
"- **`Ctrl + Enter`**: Tick or untick a task\n"
"- **`Ctrl + K`** / **`Ctrl + D`**: Delete / duplicate the line\n"
"- **`Ctrl + J`**: The scratchpad (notes/scratchpad.md); **`Ctrl + T`** adds a timestamped line\n"
"- **`Ctrl + R`**: Record a voice memo (again to stop) - its link goes in at the cursor; **`Ctrl + L`** plays it\n"
"- **`Ctrl + M`**: Voice memos: play, transcribe, delete, settings\n"
"- **`Sym + L`**: Hide the file list\n\n"
"## Everything Works From the Keyboard\n\n"
"Every screen can be used without touching it: **arrows** move, **Tab** switches area or field, "
"**Enter** opens or presses, **Space** ticks, **Left / Right** change a setting, **Esc** goes back. "
"The bottom line of each screen shows its keys.\n\n"
"## Global Keyboard Shortcuts\n\n"
"The Tab5 keyboard has no Fn key: hold **`Sym`** for system shortcuts.\n"
"Tap **`Aa`** for caps lock, hold it for Shift.\n\n"
"- **`Sym + 1`** Terminal, **`Sym + 2`** Editor, **`Sym + 3`** Tailscale, **`Sym + 4`** WireGuard, "
"**`Sym + 5`** MQTT, **`Sym + 6`** Settings\n"
"- **`Sym + Space`**: Command palette - type part of an app or command (\"doc\", \"theme\", \"reboot\"), "
"**Enter** runs it\n"
"- **`Sym + I`**: System info - battery, memory, network and CPU over the app you're in\n"
"- **`Sym + S`**: Keyboard shortcuts - the global ones and the current app's\n"
"- **`Sym + V`**: Paste the clipboard - into a text field, the Terminal's session or the Editor. "
"Copy with **`Ctrl + C`** in the Editor or **`C`** on an account in the Authenticator\n"
"- **`Sym + H`** or **`Esc`**: Home Screen (Esc goes to the remote shell in Terminal)\n"
"- **`1` .. `8`**: Launch an app from the Home Screen\n"
"- **`Sym + T`**: Toggle Dark Cyberdeck / High-Contrast Light theme\n"
"- **`Sym + -` / `Sym + +`**: Screen brightness\n"
"- **`Sym + Up` / `Sym + Down`**: Page Up / Page Down\n"
"- **`Alt + Tab`**: Switch to previous application\n\n"
"## Apps You Don't Use\n\n"
"**Settings > Apps** switches apps off, freeing the memory they take (it shows how much). "
"Changes apply after a restart (**Enter** there restarts). Hold a finger on the screen while the "
"Tab5 powers on to start with every app switched back on.\n\n"
"## Hardware Quick Reference\n\n"
"| Peripheral | Controller | Bus / Pins | Notes |\n"
"| :--- | :--- | :--- | :--- |\n"
"| SoC | ESP32-P4 | Dual RISC-V @ 400MHz | 32MB PSRAM |\n"
"| Display | 5.0\" 1280x720 | MIPI-DSI | ST7123 |\n"
"| Keyboard | A164 70-Key | Ext.Port1 I2C 0x6D | STM32F030 |\n"
"| Power | NP-F550 | INA226 I2C 0x41 | Telemetry |\n\n"
"> Distraction-free mobile engineering on the edge.\n\n"
"### Features\n\n"
"- [x] Multi-session SSH terminal\n"
"- [x] Markdown editor and SD card file browser\n"
"- [x] File sharing: add and remove files on the SD card from a browser (Settings > File Sharing)\n"
"- [x] Tailscale and WireGuard (put wg-quick .conf files in /wireguard, or press Q to scan a QR code)\n"
"- [x] MQTT monitor & publisher\n"
"- [x] Network tools (ping, DNS, port scan, Wi-Fi survey, mDNS) and a REST / webhook client\n"
"- [x] Docker / Portainer console, ADS-B radar and an offline 2FA authenticator\n";

/* Saved SSH hosts start empty: add them from the Terminal app. */
static const char *BOOKMARKS_JSON_CONTENT = "[\n]\n";

/* Disabled Jobs starters (imported explicitly, never scheduled by discovery). */
static const char *JOB_EXAMPLE_NAS =
    "version 1;\n"
    "job \"NAS health check\" {\n"
    "    trigger every 5m;\n"
    "    network.ping(host: \"nas.local\", timeout: 3s) as nas;\n"
    "    if !nas.ok {\n"
    "        system.notify(message: \"NAS offline\", level: \"warning\");\n"
    "    } else {\n"
    "        system.log(message: \"NAS responded in ${nas.latency_ms} ms\");\n"
    "    }\n"
    "}\n";

static const char *JOB_EXAMPLE_WEB =
    "version 1;\n"
    "job \"Website monitor\" {\n"
    "    trigger every 2m;\n"
    "    http.request(method: \"GET\", url: \"https://example.com/health\",\n"
    "                 timeout: 10s, max_body: 4096) as response;\n"
    "    if !response.ok || response.status != 200 {\n"
    "        system.notify(message: \"Website failed: ${response.status}\", level: \"error\");\n"
    "    }\n"
    "}\n";

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

    snprintf(path_buf, sizeof(path_buf), "%s/wireguard", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/.devos", mount_point);
    make_dir_if_missing(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/.devos/logs", mount_point);
    make_dir_if_missing(path_buf);

    /* Jobs: public source + examples, and the private revision/catalog tree. */
    snprintf(path_buf, sizeof(path_buf), "%s/jobs", mount_point);
    make_dir_if_missing(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/jobs/examples", mount_point);
    make_dir_if_missing(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/.devos/jobs", mount_point);
    make_dir_if_missing(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/.devos/jobs/revisions", mount_point);
    make_dir_if_missing(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/.devos/jobs/history", mount_point);
    make_dir_if_missing(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/.devos/jobs/drafts", mount_point);
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

    snprintf(path_buf, sizeof(path_buf), "%s/jobs/examples/nas-check.job", mount_point);
    write_file_if_missing(path_buf, JOB_EXAMPLE_NAS);

    snprintf(path_buf, sizeof(path_buf), "%s/jobs/examples/website-monitor.job", mount_point);
    write_file_if_missing(path_buf, JOB_EXAMPLE_WEB);

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
