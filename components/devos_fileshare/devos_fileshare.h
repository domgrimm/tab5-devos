#pragma once

/* devos_fileshare: the SD card as a web page (no LVGL). While it's on, any
 * browser on the same network (or over Tailscale / WireGuard) can list,
 * download, upload, rename and delete files and folders on the card, after
 * giving the password - a new random one each time sharing starts. It is
 * off after every restart; Settings > File Sharing switches it on.
 *
 * A Core 0 listener hands each connection to a short-lived worker (at most
 * DEVOS_FILESHARE_MAX_CLIENTS at once); one request per connection. HTTP
 * Basic auth (any user name), so curl works too:
 *
 *   curl -u devos:PASSWORD 'http://IP/api/list?path=/notes'
 *   curl -u devos:PASSWORD -H 'X-Devos: 1' -T a.md 'http://IP/api/file?path=/notes/a.md'
 *
 *   GET    /                               the web page
 *   GET    /api/list?path=/dir             {"path","free","total","entries":[{"name","dir","size","mtime"}]}
 *   GET    /api/file?path=/f[&dl=1]        the file (dl=1: as a download)
 *   PUT    /api/file?path=/f               upload (the body; Content-Length needed; missing
 *                                          folders are made, an existing file is replaced)
 *   DELETE /api/file?path=/p[&recursive=1] delete a file, or a folder (recursive: with its contents)
 *   POST   /api/mkdir?path=/dir            new folder
 *   POST   /api/rename?path=/a&to=/b       rename / move
 *
 * Every request except GET needs an "X-Devos: 1" header, which a web page
 * on another site can't send without asking first (CSRF). Paths are
 * relative to the card; ".." is refused.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DEVOS_FILESHARE_PORT
#ifdef ESP_PLATFORM
#define DEVOS_FILESHARE_PORT 80
#else
#define DEVOS_FILESHARE_PORT 8080      /* the simulator isn't root */
#endif
#endif
#define DEVOS_FILESHARE_MAX_CLIENTS 3

typedef struct {
    bool running;                      /* listening */
    int port;
    char password[12];                 /* "" when off */
    char error[96];                    /* why it stopped / couldn't start ("" = fine) */
    int clients;                       /* connections being served now */
    uint32_t requests, uploads, downloads, deletes;
    uint64_t bytes_in, bytes_out;      /* file data uploaded / downloaded */
    int64_t last_time;                 /* time() of the last change / download, 0 = none */
    char last[128];                    /* "Uploaded /notes/a.md (12 KB)" */
    char last_client[16];              /* its IP address */
} devos_fileshare_status_t;

/* Mutex only; nothing listens until start. */
void devos_fileshare_init(void);
/* Start sharing with a new password. false (see status.error) if there's no
 * SD card; a port that can't be opened shows up in status.error shortly. */
bool devos_fileshare_start(void);
/* Stop listening; transfers in progress are cut off. */
void devos_fileshare_stop(void);
/* Lock-free: safe from anywhere, before init too. */
bool devos_fileshare_running(void);
void devos_fileshare_status(devos_fileshare_status_t *out);

#ifdef __cplusplus
}
#endif
