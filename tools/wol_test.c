/* Host test for the Wake-on-LAN helpers (components/devos_netdiag/nd_wol.c).
 * The UDP send itself is exercised in the simulator / on device; here we check
 * the MAC parser and formatter.
 *
 *   gcc -O2 -Icomponents/devos_netdiag -Icomponents/devos_net -Icomponents/devos_config \
 *       -Imain/include tools/wol_test.c components/devos_netdiag/nd_wol.c \
 *       components/devos_netdiag/nd_common.c \
 *       -o /tmp/wol_test && /tmp/wol_test
 *
 * The two devos_net functions the send path uses are stubbed below: the
 * parser and formatter (all this test covers) never call them. */
#include "devos_netdiag.h"

#include <stdio.h>
#include <string.h>

/* Link-time stubs for the parts of nd_wol.c that touch the network stack. */
int devos_net_socket_route(int sock, uint32_t dest_ip) { (void)sock; (void)dest_ip; return 0; }
int devos_net_resolve(const char *hostname, char *out_ip, size_t out_len)
{
    (void)hostname; (void)out_ip; (void)out_len;
    return -1;
}

static int fails;

/* nd_common.c owns devos_netdiag_init(), which references the sibling engine
 * initialisers; this test only links nd_wol.c, so stub them. */
void nd_ping_init(void) {}
void nd_dns_init(void) {}
void nd_scan_init(void) {}
void nd_probe_init(void) {}

static void check_parse(const char *in, int want_rc, const uint8_t want[6])
{
    uint8_t mac[6];
    char err[64] = "";
    int rc = devos_wol_parse_mac(in, mac, err, sizeof(err));
    if (rc != want_rc) {
        printf("FAIL parse \"%s\": rc=%d want %d (%s)\n", in, rc, want_rc, err);
        fails++;
        return;
    }
    if (rc == 0 && memcmp(mac, want, 6)) {
        printf("FAIL parse \"%s\": wrong bytes\n", in);
        fails++;
        return;
    }
    printf("ok   parse \"%s\"\n", in);
}

int main(void)
{
    const uint8_t m[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

    check_parse("aa:bb:cc:dd:ee:ff", 0, m);
    check_parse("AA:BB:CC:DD:EE:FF", 0, m);
    check_parse("aa-bb-cc-dd-ee-ff", 0, m);
    check_parse("aabb.ccdd.eeff", 0, m);
    check_parse("aabbccddeeff", 0, m);
    check_parse("aabbccddeeff", 0, m);
    check_parse("", -1, NULL);
    check_parse("aa:bb:cc:dd:ee", -1, NULL);          /* too short */
    check_parse("aa:bb:cc:dd:ee:ff:00", -1, NULL);    /* too long */
    check_parse("zz:bb:cc:dd:ee:ff", -1, NULL);       /* bad digit */

    char out[24];
    devos_wol_format_mac(m, out, sizeof(out));
    if (strcmp(out, "aa:bb:cc:dd:ee:ff")) {
        printf("FAIL format: %s\n", out);
        fails++;
    } else {
        printf("ok   format\n");
    }

    /* round trip */
    uint8_t mac[6];
    char err[64];
    if (devos_wol_parse_mac(out, mac, err, sizeof(err)) != 0 || memcmp(mac, m, 6)) {
        printf("FAIL round trip\n");
        fails++;
    } else {
        printf("ok   round trip\n");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
