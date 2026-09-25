#pragma once
/* devos_conn: blocking TCP connection with optional TLS.
 *
 * TCP goes through devos_net (so MagicDNS names and 100.x addresses route
 * over the Tailscale tunnel). TLS verifies the server certificate against the
 * system CA bundle with SNI + hostname check: mbedTLS + the ESP-IDF
 * certificate bundle on the device, the host's OpenSSL (loaded at run time)
 * in the simulator. Meant for worker threads, never the UI thread.
 */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct devos_conn devos_conn_t;

#define DEVOS_CONN_TIMEOUT (-2)

/* NULL on failure (reason in err). */
devos_conn_t *devos_conn_open(const char *host, int port, bool tls, int timeout_ms, char *err, size_t errlen);
int  devos_conn_write_all(devos_conn_t *c, const void *buf, size_t len);   /* 0 ok, -1 error */
/* >0 bytes, 0 closed by peer, -1 error, DEVOS_CONN_TIMEOUT no data in time */
int  devos_conn_read(devos_conn_t *c, void *buf, size_t len, int timeout_ms);
void devos_conn_close(devos_conn_t *c);

/* Split "http(s)://host[:port][/path]" (scheme optional, default http).
 * Returns 0 and fills host/port/tls/path ("" when none). */
int devos_url_parse(const char *url, char *host, size_t hlen, int *port, bool *tls, char *path, size_t plen);

#ifdef __cplusplus
}
#endif
