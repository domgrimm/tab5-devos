/* Host-side unit test: the command palette's search
 * (components/devos_ui/devos_match.c) - what each query finds and in what
 * order, using the real app titles / keywords the palette builds.
 *
 * Compile and run:
 *   gcc -o /tmp/palette_test tools/palette_test.c components/devos_ui/devos_match.c \
 *     -Icomponents/devos_ui && /tmp/palette_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "devos_match.h"

/* title, keywords: as devos_cmdpal.c gathers them (apps: uid name category
 * subtitle; commands: fixed title + keywords) */
static const char *const E[][2] = {
    { "Terminal / SSH", "terminal Terminal systems Multi-session SSH client" },
    { "Markdown Editor", "editor Editor notes Notes, docs and files on the SD card" },
    { "Tailscale", "tailscale Tailscale network Mesh VPN" },
    { "WireGuard", "wireguard WireGuard network Tunnels from wg-quick configs" },
    { "MQTT", "mqtt MQTT network Broker monitor & publisher" },
    { "Settings", "settings Settings system Wi-Fi, file sharing, display & system" },
    { "Network", "netdiag Network network Ping, DNS, port scan" },
    { "REST", "rest REST network REST & webhook client" },
    { "Docker", "docker Docker network Containers" },
    { "ADS-B Radar", "adsb ADS-B network Aircraft overhead" },
    { "Authenticator", "totp 2FA security Offline TOTP codes" },
    { "Home Screen", "launcher Launcher system" },
    { "Share the SD card (File Sharing)", "File Sharing share sharing sd card files web browser upload download" },
    { "Check for updates", "Check for updates update upgrade ota firmware install" },
    { "Open the scratchpad", "Open the scratchpad scratch scratchpad notes journal log memo jot" },
    { "Switch to the light theme", "Switch theme theme dark light colours colors mode" },
    { "System info", "System info hud info battery power memory ram psram cpu load wifi ip uptime stats" },
    { "Restart the Tab5", "Restart the Tab5 reboot restart reset" },
};
#define N ((int)(sizeof(E) / sizeof(E[0])))

/* Title of the best match (first on a tie, as the palette sorts), NULL = none. */
static const char *best(const char *q)
{
    int bi = -1, bs = -1;
    for (int i = 0; i < N; i++) {
        int s = devos_match_score(q, E[i][0], E[i][1]);
        if (s > bs) {
            bs = s;
            bi = i;
        }
    }
    return bi >= 0 && bs >= 0 ? E[bi][0] : NULL;
}

static int count(const char *q)
{
    int n = 0;
    for (int i = 0; i < N; i++) n += devos_match_score(q, E[i][0], E[i][1]) >= 0;
    return n;
}

static void expect(const char *q, const char *title)
{
    const char *got = best(q);
    if (!got || strcmp(got, title) != 0) {
        printf("FAIL: \"%s\" -> %s (want %s)\n", q, got ? got : "(nothing)", title);
        assert(0);
    }
}

int main(void)
{
    /* the examples the palette was specified with */
    expect("doc", "Docker");
    expect("totp", "Authenticator");
    expect("auth", "Authenticator");
    expect("adsb", "ADS-B Radar");
    expect("radar", "ADS-B Radar");
    expect("theme", "Switch to the light theme");
    expect("dark", "Switch to the light theme");
    expect("light", "Switch to the light theme");
    expect("share", "Share the SD card (File Sharing)");
    expect("reboot", "Restart the Tab5");
    expect("scratch", "Open the scratchpad");
    /* case, punctuation, several words, letters in order */
    expect("DOCKER", "Docker");
    expect("ads-b", "ADS-B Radar");
    expect("ssh", "Terminal / SSH");
    expect("wire", "WireGuard");
    expect("wg", "WireGuard");            /* keyword word start: "wg-quick" */
    expect("home", "Home Screen");
    expect("check up", "Check for updates");
    expect("dkr", "Docker");              /* subsequence */
    expect("info", "System info");
    expect("ram", "System info");
    /* little words aren't word starts: "the" is the theme, not "Open the ..." */
    expect("the", "Switch to the light theme");
    /* title start beats a keyword: "set" is Settings, not "reset" */
    expect("set", "Settings");
    /* every word must match */
    assert(count("docker zzz") == 0);
    assert(count("qqqq") == 0);
    /* an empty query lists everything, all scoring 0 */
    assert(count("") == N);
    assert(count("   ") == N);
    assert(devos_match_score("", "Docker", NULL) == 0);
    assert(devos_match_score(NULL, "Docker", NULL) == 0);
    /* one-letter queries don't match by letters in order (too loose) */
    assert(devos_match_score("q", "Docker", "") < 0);
    printf("palette_test: all passed\n");
    return 0;
}
