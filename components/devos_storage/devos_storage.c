#include "devos_storage.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#endif

static bool storage_mounted = false;
static uint32_t total_mb = 31200;
static uint32_t free_mb = 29412;

static const char *WELCOME_MD_CONTENT =
"# Welcome to devOS on M5Stack Tab5!\n\n"
"devOS is a developer-focused mobile cyberdeck firmware for the M5Stack Tab5 with A164 keyboard.\n\n"
"## Global Keyboard Shortcuts\n\n"
"- **`1` .. `6`**: Quick launch app from Home Screen\n"
"- **`Fn + H`** or **`Esc`**: Global return to Home Screen\n"
"- **`Fn + 1` .. `Fn + 6`**: Instant app switch from anywhere\n"
"- **`Fn + T`**: Toggle Dark Cyberdeck / High-Contrast Light theme\n"
"- **`Fn + F`**: Toggle Focus Mode in AI Agent & Antigravity views\n"
"- **`Fn + [`**: Toggle Left Sidebar (Sessions, Subagents, Bookmarks)\n"
"- **`Fn + ]`**: Toggle Right Inspector (Files, Diffs, Artifacts)\n"
"- **`Alt + Tab`**: Switch to previous application\n\n"
"## Tailscale Enrollment\n\n"
"Open the Tailscale app (Card 4) and provide your pre-authenticated auth key (`tskey-auth-...`).\n\n"
"Happy Hacking!\n";

static const char *BOOKMARKS_JSON_CONTENT =
"[\n"
"  {\n"
"    \"alias\": \"Workstation\",\n"
"    \"host\": \"100.77.11.92\",\n"
"    \"port\": 22,\n"
"    \"user\": \"dom\",\n"
"    \"auth\": \"key\",\n"
"    \"key_path\": \"/sdcard/.ssh/id_ed25519\"\n"
"  },\n"
"  {\n"
"    \"alias\": \"Dev Cluster\",\n"
"    \"host\": \"100.64.1.2\",\n"
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

    snprintf(path_buf, sizeof(path_buf), "%s/.devos/version.txt", mount_point);
    write_file_if_missing(path_buf, DEVOS_VERSION_STR "\n");

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
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(TAB5_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK) {
        storage_mounted = true;
        total_mb = (uint32_t)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        free_mb = total_mb * 9 / 10;
        devos_storage_bootstrap(TAB5_SD_MOUNT_POINT);
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
