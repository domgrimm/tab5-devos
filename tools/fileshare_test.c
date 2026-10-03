/* Host end-to-end test for devos_fileshare: starts the server on a loopback
 * port against a scratch SD card and drives it over real sockets - auth,
 * CSRF header, list / download / upload / mkdir / rename / delete, path
 * traversal, 100-continue, stop / restart with a new password.
 *
 *   gcc -Wall -Wextra -o /tmp/fileshare_test tools/fileshare_test.c \
 *     components/devos_fileshare/devos_fileshare.c components/devos_fileshare/fileshare_page.c \
 *     components/devos_net/devos_net.c components/devos_storage/devos_storage.c \
 *     components/devos_crypto/devos_crypto.c components/devos_json/devos_json.c \
 *     components/devos_core/devos_clipboard.c \
 *     -Icomponents/devos_fileshare -Icomponents/devos_net -Icomponents/devos_storage \
 *     -Icomponents/devos_crypto -Icomponents/devos_json -Icomponents/devos_tailnet \
 *     -Icomponents/devos_core -Icomponents/devos_config/include \
 *     -DDEVOS_FILESHARE_PORT=18480 -lpthread && /tmp/fileshare_test
 *
 * It makes (and leaves) its own scratch directory under /tmp and runs there.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "devos_fileshare.h"
#include "devos_storage.h"
#include "devos_clipboard.h"

/* devos_net's resolver asks the tailnet first; not here */
int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    (void)name; (void)out_ip; (void)out_len;
    return -1;
}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char s_pw[16];

static void b64(const char *in, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)(unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DEVOS_FILESHARE_PORT) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

typedef struct {
    int status;
    char head[4096];
    char *body;
    size_t len;
} resp_t;

static void read_resp(int fd, resp_t *r, char *pre, size_t pre_len)
{
    size_t cap = 1 << 16, n = pre_len;
    char *buf = malloc(cap + 1);
    memcpy(buf, pre, pre_len);
    for (;;) {
        if (n == cap) buf = realloc(buf, (cap *= 2) + 1);
        ssize_t got = recv(fd, buf + n, cap - n, 0);
        if (got <= 0) break;
        n += (size_t)got;
    }
    buf[n] = '\0';
    char *p = buf;
    r->status = 0;
    for (;;) {                                  /* skip "100 Continue" */
        sscanf(p, "HTTP/1.1 %d", &r->status);
        char *e = strstr(p, "\r\n\r\n");
        if (!e) {
            r->head[0] = '\0';
            r->body = strdup("");
            r->len = 0;
            free(buf);
            return;
        }
        if (r->status != 100) {
            size_t hl = (size_t)(e - p);
            if (hl >= sizeof(r->head)) hl = sizeof(r->head) - 1;
            memcpy(r->head, p, hl);
            r->head[hl] = '\0';
            r->len = n - (size_t)(e + 4 - buf);
            r->body = malloc(r->len + 1);
            memcpy(r->body, e + 4, r->len + 1);
            free(buf);
            return;
        }
        p = e + 4;
    }
}

/* One request. pw NULL: no Authorization; csrf adds X-Devos. */
static int http(const char *method, const char *target, const char *pw, bool csrf, const void *body, size_t blen,
                const char *extra, resp_t *r)
{
    int fd = dial();
    if (fd < 0) {
        r->status = -1;
        r->body = strdup("");
        r->len = 0;
        r->head[0] = '\0';
        return -1;
    }
    size_t cap = 4096 + (extra ? strlen(extra) : 0);
    char *req = malloc(cap), auth[160] = "";
    if (pw) {
        char cred[64], enc[100];
        snprintf(cred, sizeof(cred), "devos:%s", pw);
        b64(cred, enc);
        snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", enc);
    }
    int n = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s%s", method, target, auth,
                     csrf ? "X-Devos: 1\r\n" : "", extra ? extra : "");
    if (body) n += snprintf(req + n, cap - (size_t)n, "Content-Length: %zu\r\n", blen);
    n += snprintf(req + n, cap - (size_t)n, "\r\n");
    send(fd, req, (size_t)n, MSG_NOSIGNAL);
    free(req);
    if (body && blen) send(fd, body, blen, MSG_NOSIGNAL);
    read_resp(fd, r, NULL, 0);
    close(fd);
    return r->status;
}

static int simple(const char *method, const char *target, resp_t *r)
{
    return http(method, target, s_pw, true, NULL, 0, NULL, r);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';
    if (len) *len = got;
    return b;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void wait_running(bool on)
{
    for (int i = 0; i < 100 && devos_fileshare_running() != on; i++) usleep(50 * 1000);
}

int main(void)
{
    char dir[] = "/tmp/devos_sharetest.XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) {
        perror("scratch dir");
        return 1;
    }
    printf("scratch dir %s, port %d\n", dir, DEVOS_FILESHARE_PORT);
    devos_storage_init();                       /* ./sim_sdcard with the starter files */
    devos_fileshare_init();

    devos_fileshare_status_t st;
    devos_fileshare_status(&st);
    CHECK(!st.running && !st.password[0] && !devos_fileshare_running());
    CHECK(devos_fileshare_start());
    wait_running(true);
    CHECK(devos_fileshare_running());
    devos_fileshare_status(&st);
    CHECK(strlen(st.password) == 9 && st.password[4] == '-');
    snprintf(s_pw, sizeof(s_pw), "%s", st.password);
    resp_t r;

    /* auth */
    CHECK(http("GET", "/", NULL, false, NULL, 0, NULL, &r) == 401);
    CHECK(strstr(r.head, "WWW-Authenticate: Basic") != NULL);
    free(r.body);
    time_t t0 = time(NULL);
    CHECK(http("GET", "/", "wrong-pass", false, NULL, 0, NULL, &r) == 401);
    CHECK(time(NULL) - t0 >= 1);               /* guessing is slowed down */
    free(r.body);
    CHECK(http("GET", "/", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(strstr(r.head, "text/html") && strstr(r.body, "<title>devOS SD card</title>"));
    CHECK(strstr(r.head, "frame-ancestors 'none'") != NULL);
    free(r.body);

    /* list the card */
    CHECK(http("GET", "/api/list?path=/", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(strstr(r.body, "\"name\":\"notes\",\"dir\":true") != NULL);
    CHECK(strstr(r.body, "\"path\":\"/\"") != NULL);
    free(r.body);
    CHECK(http("GET", "/api/list?path=/notes", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(strstr(r.body, "\"name\":\"welcome.md\",\"dir\":false,\"size\":") != NULL);
    free(r.body);
    CHECK(simple("GET", "/api/list?path=/nope", &r) == 404);
    free(r.body);
    CHECK(simple("GET", "/api/list?path=/notes/welcome.md", &r) == 400);
    free(r.body);

    /* CSRF: changes need X-Devos */
    CHECK(http("PUT", "/api/file?path=/up/a.txt", s_pw, false, "hi", 2, NULL, &r) == 403);
    free(r.body);
    CHECK(http("POST", "/api/mkdir?path=/x", s_pw, false, NULL, 0, NULL, &r) == 403);
    free(r.body);
    CHECK(!exists("sim_sdcard/up") && !exists("sim_sdcard/x"));

    /* upload into a folder that doesn't exist yet, a name with a space */
    CHECK(http("PUT", "/api/file?path=/up/sub/a%20b.txt", s_pw, true, "hello", 5, NULL, &r) == 201);
    free(r.body);
    char *got = slurp("sim_sdcard/up/sub/a b.txt", NULL);
    CHECK(got && strcmp(got, "hello") == 0);
    free(got);
    CHECK(!exists("sim_sdcard/up/sub/a b.txt.part~"));
    /* replace it */
    CHECK(http("PUT", "/api/file?path=/up/sub/a%20b.txt", s_pw, true, "bye", 3, NULL, &r) == 200);
    free(r.body);
    got = slurp("sim_sdcard/up/sub/a b.txt", NULL);
    CHECK(got && strcmp(got, "bye") == 0);
    free(got);
    CHECK(http("PUT", "/api/file?path=/up/empty.txt", s_pw, true, "", 0, NULL, &r) == 201);
    free(r.body);
    CHECK(exists("sim_sdcard/up/empty.txt"));
    CHECK(http("PUT", "/api/file?path=/up", s_pw, true, "x", 1, NULL, &r) == 409);   /* a folder */
    free(r.body);
    CHECK(http("PUT", "/api/file?path=/up/c.txt", s_pw, true, NULL, 0, NULL, &r) == 411);
    free(r.body);
    CHECK(http("PUT", "/api/file?path=/", s_pw, true, "x", 1, NULL, &r) == 400);
    free(r.body);

    /* a 3 MB binary round trip */
    size_t big = (3u << 20) + 12345;          /* not a whole number of chunks */
    unsigned char *blob = malloc(big);
    for (size_t i = 0; i < big; i++) blob[i] = (unsigned char)(i * 131 + (i >> 9));
    CHECK(http("PUT", "/api/file?path=/up/blob.bin", s_pw, true, blob, big, NULL, &r) == 201);
    free(r.body);
    CHECK(http("GET", "/api/file?path=/up/blob.bin", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(r.len == big && memcmp(r.body, blob, big) == 0);
    CHECK(strstr(r.head, "application/octet-stream") && strstr(r.head, "sandbox"));
    free(r.body);

    /* 100-continue: headers first, the body once the server says so */
    {
        int fd = dial();
        char cred[64], enc[100], req[512];
        snprintf(cred, sizeof(cred), "u:%s", s_pw);
        b64(cred, enc);
        int n = snprintf(req, sizeof(req),
                         "PUT /api/file?path=/up/cont.txt HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n"
                         "X-Devos: 1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n", enc);
        send(fd, req, (size_t)n, MSG_NOSIGNAL);
        char first[64] = "";
        ssize_t k = recv(fd, first, sizeof(first) - 1, 0);
        if (k > 0) first[k] = '\0';
        CHECK(strncmp(first, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0);
        send(fd, "abcd", 4, MSG_NOSIGNAL);
        read_resp(fd, &r, first + (k >= 25 ? 25 : 0), k >= 25 ? (size_t)k - 25 : 0);
        close(fd);
        CHECK(r.status == 201);
        free(r.body);
        got = slurp("sim_sdcard/up/cont.txt", NULL);
        CHECK(got && strcmp(got, "abcd") == 0);
        free(got);
    }

    /* download: inline text / as a file */
    CHECK(http("GET", "/api/file?path=/up/sub/a%20b.txt", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(r.len == 3 && strcmp(r.body, "bye") == 0);
    CHECK(strstr(r.head, "text/plain") && strstr(r.head, "inline; filename*=UTF-8''a%20b.txt"));
    free(r.body);
    CHECK(http("GET", "/api/file?dl=1&path=/up/sub/a%20b.txt", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(strstr(r.head, "attachment") != NULL);
    free(r.body);
    CHECK(simple("GET", "/api/file?path=/up/none.txt", &r) == 404);
    free(r.body);

    /* HTML never runs from the card */
    CHECK(http("PUT", "/api/file?path=/up/x.html", s_pw, true, "<script>1</script>", 18, NULL, &r) == 201);
    free(r.body);
    CHECK(simple("GET", "/api/file?path=/up/x.html", &r) == 200);
    CHECK(strstr(r.head, "text/plain") && !strstr(r.head, "text/html"));
    free(r.body);

    /* paths that escape the card or FAT can't store */
    const char *bad[] = { "/../etc/passwd", "/notes/../../x", "/%2e%2e/x", "..", "/a/..", "/a%5cb", "/a:b",
                          "/x%00y", "/%01", "/...", "/%zz" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char t[128];
        snprintf(t, sizeof(t), "/api/file?path=%s", bad[i]);
        int code = http("PUT", t, s_pw, true, "x", 1, NULL, &r);
        if (code != 400) printf("  path %s -> %d\n", bad[i], code);
        CHECK(code == 400);
        free(r.body);
    }
    CHECK(!exists("x") && !exists("../x"));
    /* "." parts and doubled slashes are fine */
    CHECK(simple("GET", "/api/file?path=//up/./sub//a%20b.txt", &r) == 200);
    free(r.body);
    CHECK(simple("GET", "/api/list", &r) == 400);                /* no path */
    free(r.body);

    /* folders */
    CHECK(simple("POST", "/api/mkdir?path=/newdir", &r) == 201);
    free(r.body);
    CHECK(exists("sim_sdcard/newdir"));
    CHECK(simple("POST", "/api/mkdir?path=/newdir", &r) == 409);
    free(r.body);
    CHECK(simple("GET", "/api/mkdir?path=/newdir2", &r) == 405);
    free(r.body);

    /* rename / move */
    CHECK(simple("POST", "/api/rename?path=/up/sub/a%20b.txt&to=/newdir/b.txt", &r) == 200);
    free(r.body);
    CHECK(!exists("sim_sdcard/up/sub/a b.txt") && exists("sim_sdcard/newdir/b.txt"));
    CHECK(simple("POST", "/api/rename?path=/up/empty.txt&to=/newdir/b.txt", &r) == 409);
    free(r.body);
    CHECK(simple("POST", "/api/rename?path=/newdir&to=/newdir/inner", &r) == 400);
    free(r.body);
    CHECK(simple("POST", "/api/rename?path=/missing&to=/m2", &r) == 404);
    free(r.body);
    CHECK(simple("POST", "/api/rename?path=/up/cont.txt&to=/", &r) == 400);
    free(r.body);

    /* delete */
    CHECK(simple("DELETE", "/api/file?path=/newdir", &r) == 409);   /* not empty */
    free(r.body);
    CHECK(exists("sim_sdcard/newdir/b.txt"));
    CHECK(simple("DELETE", "/api/file?path=/up&recursive=1", &r) == 200);
    free(r.body);
    CHECK(!exists("sim_sdcard/up"));
    CHECK(simple("DELETE", "/api/file?path=/newdir/b.txt", &r) == 200);
    free(r.body);
    CHECK(simple("DELETE", "/api/file?path=/newdir", &r) == 200);
    free(r.body);
    CHECK(!exists("sim_sdcard/newdir"));
    CHECK(simple("DELETE", "/api/file?path=/&recursive=1", &r) == 400);
    free(r.body);
    CHECK(exists("sim_sdcard/notes/welcome.md"));
    CHECK(simple("DELETE", "/api/file?path=/gone", &r) == 404);
    free(r.body);

    /* Universal Clipboard: the page sends text, the Tab5 (and a file) keep it */
    CHECK(http("GET", "/api/clipboard", s_pw, false, NULL, 0, NULL, &r) == 200);
    CHECK(strstr(r.body, "\"text\":\"\"") && strstr(r.body, "\"length\":0"));
    free(r.body);
    CHECK(http("POST", "/api/clipboard", s_pw, false, "no csrf", 7, NULL, &r) == 403);
    free(r.body);
    CHECK(http("POST", "/api/clipboard", s_pw, true, "hello clipboard", 15, NULL, &r) == 200);
    CHECK(strstr(r.body, "\"ok\":true") && strstr(r.body, "\"length\":15"));
    free(r.body);
    CHECK(devos_clipboard_get(NULL) && strcmp(devos_clipboard_get(NULL), "hello clipboard") == 0);
    got = slurp("sim_sdcard/.devos/clipboard.txt", NULL);
    CHECK(got && strcmp(got, "hello clipboard") == 0);
    free(got);
    /* read-back returns exactly what was set */
    CHECK(simple("GET", "/api/clipboard", &r) == 200);
    CHECK(strstr(r.body, "\"text\":\"hello clipboard\"") && strstr(r.body, "\"length\":15"));
    free(r.body);
    /* special characters survive JSON round-trip */
    {
        char *txt = "quote \" backslash \\ newline \n tab \t end";
        CHECK(http("POST", "/api/clipboard", s_pw, true, txt, strlen(txt), NULL, &r) == 200);
        free(r.body);
        CHECK(devos_clipboard_get(NULL) && strcmp(devos_clipboard_get(NULL), txt) == 0);
        CHECK(simple("GET", "/api/clipboard", &r) == 200);
        CHECK(strstr(r.body, "\\\"") && strstr(r.body, "\\\\") && strstr(r.body, "\\n"));
        free(r.body);
        got = slurp("sim_sdcard/.devos/clipboard.txt", NULL);
        CHECK(got && strcmp(got, txt) == 0);
        free(got);
    }
    /* an empty paste clears it (both the clipboard and the file) */
    CHECK(http("POST", "/api/clipboard", s_pw, true, "", 0, NULL, &r) == 200);
    CHECK(strstr(r.body, "\"length\":0"));
    free(r.body);
    CHECK(strcmp(devos_clipboard_get(NULL), "") == 0);
    CHECK(exists("sim_sdcard/.devos/clipboard.txt") && slurp("sim_sdcard/.devos/clipboard.txt", NULL)[0] == '\0');
    free(slurp("sim_sdcard/.devos/clipboard.txt", NULL));
    /* a paste that spans several IO_CHUNK reads (headers + body split) */
    {
        size_t many = 100 * 1024;                    /* > 64 KB: exceeds the cap */
        char *bulk = malloc(many);
        for (size_t i = 0; i < many; i++) bulk[i] = (char)('a' + (i % 26));
        CHECK(http("POST", "/api/clipboard", s_pw, true, bulk, many, NULL, &r) == 413);
        free(r.body);
        /* just under the cap, arriving in pieces: kept in full and on the card */
        size_t okn = 40000;
        char *under = malloc(okn);
        for (size_t i = 0; i < okn; i++) under[i] = (char)('A' + (i % 26));
        CHECK(http("POST", "/api/clipboard", s_pw, true, under, okn, NULL, &r) == 200);
        CHECK(strstr(r.body, "\"length\":40000"));
        free(r.body);
        size_t cl = 0;
        const char *ct = devos_clipboard_get(&cl);
        CHECK(cl == okn && ct && memcmp(ct, under, okn) == 0);
        char *file = slurp("sim_sdcard/.devos/clipboard.txt", NULL);
        CHECK(file && strlen(file) == okn && memcmp(file, under, okn) == 0);
        free(file);
        free(bulk);
        free(under);
    }
    CHECK(simple("DELETE", "/api/clipboard", &r) == 405);
    free(r.body);
    CHECK(simple("GET", "/api/clipboard", &r) == 200);   /* still the 40 KB paste */
    CHECK(strstr(r.body, "\"length\":40000"));
    free(r.body);

    /* odds and ends */
    CHECK(simple("GET", "/nope", &r) == 404);
    free(r.body);
    {
        char *hdr = malloc(9000);
        memset(hdr, 'a', 8990);
        memcpy(hdr, "X-Big: ", 7);
        strcpy(hdr + 8990, "\r\n");
        CHECK(http("GET", "/", s_pw, false, NULL, 0, hdr, &r) == 431);
        free(r.body);
        free(hdr);
    }
    /* spare connections that never send anything don't block others */
    int idle1 = dial(), idle2 = dial();
    usleep(100 * 1000);
    CHECK(simple("GET", "/api/list?path=/", &r) == 200);
    free(r.body);
    close(idle1);
    close(idle2);

    devos_fileshare_status(&st);
    CHECK(st.uploads == 6 && st.downloads == 5 && st.deletes == 3);
    CHECK(st.clipboards == 4);                   /* hello, special chars, empty, 40 KB */
    CHECK(st.bytes_in >= big && st.bytes_out >= big);
    CHECK(strcmp(st.last_client, "127.0.0.1") == 0 && st.last[0]);
    printf("  last: %s (%s)  requests %u\n", st.last, st.last_client, st.requests);

    /* stop: port closed, password gone */
    devos_fileshare_stop();
    wait_running(false);
    CHECK(!devos_fileshare_running());
    CHECK(dial() < 0);
    devos_fileshare_status(&st);
    CHECK(!st.password[0]);

    /* restart: a new password; the old one no longer works */
    CHECK(devos_fileshare_start());
    wait_running(true);
    devos_fileshare_status(&st);
    CHECK(st.running && strlen(st.password) == 9);
    if (strcmp(st.password, s_pw) != 0) {
        CHECK(http("GET", "/api/list?path=/", s_pw, false, NULL, 0, NULL, &r) == 401);
        free(r.body);
    }
    snprintf(s_pw, sizeof(s_pw), "%s", st.password);
    CHECK(simple("GET", "/api/list?path=/", &r) == 200);
    free(r.body);
    /* quick off / on keeps serving */
    devos_fileshare_stop();
    CHECK(devos_fileshare_start());
    usleep(800 * 1000);
    wait_running(true);
    devos_fileshare_status(&st);
    snprintf(s_pw, sizeof(s_pw), "%s", st.password);
    CHECK(simple("GET", "/api/list?path=/", &r) == 200);
    free(r.body);
    devos_fileshare_stop();
    wait_running(false);

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
