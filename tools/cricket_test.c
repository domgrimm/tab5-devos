/* Host unit test for devos_cricket's parsers and devos_json_member().
 *
 *   gcc -o cricket_test tools/cricket_test.c \
 *     components/devos_cricket/devos_cricket.c \
 *     components/devos_json/devos_json.c \
 *     -DDEVOS_CRICKET_NO_NET -Icomponents/devos_cricket -Icomponents/devos_json && ./cricket_test
 *
 * Optional: ./cricket_test header.json summary.json parses real API replies
 * (saved with curl) and prints what it found.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devos_cricket.h"
#include "devos_json.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char LIST[] =
    "{\"sports\":[{\"id\":\"200\",\"name\":\"Cricket\",\"leagues\":["
    "{\"id\":\"8039\",\"name\":\"World Cup\",\"events\":[{"
    "\"id\":\"1144530\",\"date\":\"2019-07-14T09:30:00Z\",\"name\":\"England v New Zealand\","
    "\"title\":\"Final\",\"class\":{\"name\":\"One-Day Internationals\",\"generalClassCard\":\"ODI\"},"
    "\"location\":\"Lord's, London\",\"status\":\"post\",\"summary\":\"Result\","
    "\"fullStatus\":{\"longSummary\":\"Match tied (England won the boundary count)\",\"type\":{\"state\":\"post\"}},"
    "\"competitors\":[{\"id\":\"1\",\"abbreviation\":\"ENG\",\"displayName\":\"England\",\"score\":\"241 (50 ov)\",\"winner\":true},"
    "{\"id\":\"5\",\"abbreviation\":\"NZ\",\"displayName\":\"New Zealand\",\"score\":\"241/8\",\"winner\":false}]}]},"
    "{\"id\":8204,\"name\":\"County Championship Division Two\",\"events\":[{"
    "\"id\":\"1513451\",\"date\":\"2026-09-24T09:30Z\",\"title\":\"Match 52\",\"status\":\"in\","
    "\"fullStatus\":{\"longSummary\":\"Lancashire trail by 197 runs\"},"
    "\"competitors\":[{\"abbreviation\":\"LAN\",\"displayName\":\"Lancashire\",\"score\":\"296/5 (120 ov)\"},"
    "{\"abbreviation\":\"DUR\",\"displayName\":\"Durham\",\"score\":\"493\"}]}]}]}]}";

/* Two players, one innings each side, in the summary endpoint's shape. */
/* ST / STS end in a comma; STATS closes the list with an unused stat */
#define STATS(list) "\"statistics\":{\"categories\":[{\"name\":\"general\",\"stats\":[" list "{\"name\":\"_\",\"value\":0}]}]}"
#define ST(n, v) "{\"name\":\"" n "\",\"value\":" #v ",\"displayValue\":\"" #v "\"},"
#define STS(n, s) "{\"name\":\"" n "\",\"value\":0,\"displayValue\":\"" s "\"},"
static const char CARD[] =
    "{\"notes\":[{\"type\":\"season\",\"text\":\"2019\"},{\"type\":\"toss\",\"text\":\"New Zealand , elected to bat first\"}],"
    "\"gameInfo\":{\"venue\":{\"id\":\"57129\",\"fullName\":\"Lord's, London\"}},"
    "\"rosters\":["
    "{\"team\":{\"id\":\"1\",\"displayName\":\"England\"},\"roster\":["
    " {\"athlete\":{\"name\":\"Ben Stokes\",\"battingName\":\"BA Stokes\"},\"linescores\":["
    "  {\"period\":1,\"linescores\":[{\"order\":0," STATS(ST("inningsBowled", 0)) "}]},"
    "  {\"period\":2,\"linescores\":[{\"order\":5," STATS(ST("batted", 1) ST("battingPosition", 5) ST("runs", 84) ST("ballsFaced", 98) ST("fours", 5) ST("sixes", 2) ST("dismissal", 12) STS("dismissalCard", "not out")) "}]}]},"
    " {\"athlete\":{\"battingName\":\"JC Archer\"},\"linescores\":["
    "  {\"period\":1,\"linescores\":[{\"order\":0," STATS(ST("inningsBowled", 1) ST("bowlingPosition", 3) STS("overs", "10") ST("maidens", 0) ST("conceded", 42) ST("wickets", 1) ST("economyRate", 4.2)) "}]}]}]},"
    "{\"team\":{\"id\":\"5\",\"displayName\":\"New Zealand\"},\"roster\":["
    " {\"athlete\":{\"battingName\":\"KS Williamson\"},\"linescores\":["
    "  {\"period\":1,\"linescores\":[{\"order\":3," STATS(ST("batted", 1) ST("battingPosition", 3) ST("runs", 30) ST("ballsFaced", 53) ST("fours", 2) ST("sixes", 0) ST("dismissal", 1) STS("dismissalCard", "c")) "}]}]},"
    " {\"athlete\":{\"battingName\":\"HM Nicholls\"},\"linescores\":["
    "  {\"period\":1,\"linescores\":[{\"order\":2," STATS(ST("batted", 1) ST("battingPosition", 2) ST("runs", 55) ST("ballsFaced", 77) ST("fours", 4) ST("sixes", 0) ST("dismissal", 2) STS("dismissalCard", "")) "}]}]}]}],"
    "\"header\":{\"id\":\"1144530\",\"name\":\"England v New Zealand\",\"league\":{\"name\":\"World Cup\"},"
    "\"competitions\":[{\"description\":\"Final\",\"status\":{\"summary\":\"Match tied (England won the boundary count)\","
    "\"type\":{\"state\":\"post\"}},\"competitors\":["
    "{\"team\":{\"displayName\":\"England\"},\"linescores\":[{\"period\":1,\"runs\":0},"
    " {\"period\":2,\"runs\":241,\"wickets\":10,\"overs\":50.0,\"score\":\"241 (50 ov)\",\"description\":\"all out\"}]},"
    "{\"team\":{\"displayName\":\"New Zealand\"},\"linescores\":["
    " {\"period\":1,\"runs\":241,\"wickets\":8,\"overs\":50.0,\"score\":\"241/8 (50 ov)\",\"description\":\"complete\"}]}]}]}}";

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static void print_card(const devos_cricket_card_t *c)
{
    printf("%s | %s | %s | %s | %d innings\n", c->title, c->venue, c->toss, c->result, c->ninn);
    for (int i = 0; i < c->ninn; i++) {
        const devos_cricket_innings_t *in = &c->inn[i];
        printf("  %s %s  (%d bat, %d bowl)\n", in->team, in->total, in->nbat, in->nbowl);
        for (int k = 0; k < in->nbat; k++)
            printf("    %-24s %-8s %3d (%d) %d/%d\n", in->bat[k].name, in->bat[k].how, in->bat[k].runs, in->bat[k].balls,
                   in->bat[k].fours, in->bat[k].sixes);
        for (int k = 0; k < in->nbowl; k++)
            printf("    %-24s %s-%d-%d-%d\n", in->bowl[k].name, in->bowl[k].overs, in->bowl[k].maidens, in->bowl[k].runs,
                   in->bowl[k].wickets);
    }
}

int main(int argc, char **argv)
{
    /* devos_json_member skips nested objects */
    const char *o = "{\"class\":{\"name\":\"T20\"},\"list\":[{\"name\":\"x\"}],\"name\":\"Final\",\"n\":\"42\",\"m\":7.5}";
    const char *oe = o + strlen(o);
    char buf[32];
    double d;
    CHECK(devos_json_member_str(o, oe, "name", buf, sizeof(buf)) == 0 && !strcmp(buf, "Final"));
    CHECK(devos_json_member_num(o, oe, "n", &d) && d == 42);
    CHECK(devos_json_member_num(o, oe, "m", &d) && d == 7.5);
    CHECK(devos_json_member(o, oe, "missing") == NULL);
    CHECK(devos_json_member_str(o, oe, "class", buf, sizeof(buf)) == -1);

    /* dates */
    CHECK(devos_cricket_date_add(20190714, 1) == 20190715);
    CHECK(devos_cricket_date_add(20240301, -1) == 20240229);
    CHECK(devos_cricket_date_add(20261231, 1) == 20270101);

    /* match list */
    devos_cricket_match_t *m = calloc(DEVOS_CRICKET_MAX_MATCHES, sizeof(*m));
    int n = devos_cricket_parse_list(LIST, sizeof(LIST) - 1, m, DEVOS_CRICKET_MAX_MATCHES);
    CHECK(n == 2);
    CHECK(!strcmp(m[0].event, "1144530") && !strcmp(m[0].league, "8039") && !strcmp(m[0].series, "World Cup"));
    CHECK(!strcmp(m[0].title, "Final") && !strcmp(m[0].format, "ODI") && !strcmp(m[0].venue, "Lord's, London"));
    CHECK(m[0].state == DEVOS_CRICKET_DONE && strstr(m[0].status, "boundary count"));
    CHECK(!strcmp(m[0].side[0].abbr, "ENG") && m[0].side[0].winner && !m[0].side[1].winner);
    CHECK(!strcmp(m[0].side[1].score, "241/8"));
    CHECK(m[0].start == 1563096600);
    CHECK(!strcmp(m[1].league, "8204") && m[1].state == DEVOS_CRICKET_LIVE && !strcmp(m[1].side[1].name, "Durham"));

    /* scorecard */
    devos_cricket_card_t *c = calloc(1, sizeof(*c));
    CHECK(devos_cricket_parse_card(CARD, sizeof(CARD) - 1, c));
    CHECK(!strcmp(c->title, "England v New Zealand - Final"));
    CHECK(!strcmp(c->venue, "Lord's, London") && strstr(c->toss, "elected to bat"));
    CHECK(strstr(c->result, "tied") && !c->live);
    CHECK(c->ninn == 2);
    CHECK(!strcmp(c->inn[0].team, "New Zealand") && !strcmp(c->inn[0].total, "241/8 (50 ov)"));
    CHECK(c->inn[0].nbat == 2 && !strcmp(c->inn[0].bat[0].name, "HM Nicholls"));     /* sorted by position */
    CHECK(!strcmp(c->inn[0].bat[0].how, "b") && !strcmp(c->inn[0].bat[1].how, "c") && c->inn[0].bat[1].runs == 30);
    CHECK(c->inn[0].nbowl == 1 && !strcmp(c->inn[0].bowl[0].name, "JC Archer") && c->inn[0].bowl[0].wickets == 1);
    CHECK(!strcmp(c->inn[0].bowl[0].overs, "10") && c->inn[0].bowl[0].runs == 42);
    CHECK(!strcmp(c->inn[1].team, "England") && !strcmp(c->inn[1].total, "241 (50 ov) all out"));
    CHECK(c->inn[1].nbat == 1 && !strcmp(c->inn[1].bat[0].how, "not out") && c->inn[1].bat[0].sixes == 2);

    /* real replies, if given */
    if (argc > 1) {
        size_t len;
        char *js = slurp(argv[1], &len);
        n = js ? devos_cricket_parse_list(js, len, m, DEVOS_CRICKET_MAX_MATCHES) : -1;
        printf("%s: %d matches\n", argv[1], n);
        for (int i = 0; i < n && i < 8; i++)
            printf("  [%s/%s] %s %s | %s %s v %s %s | %s\n", m[i].league, m[i].event, m[i].series, m[i].title,
                   m[i].side[0].abbr, m[i].side[0].score, m[i].side[1].abbr, m[i].side[1].score, m[i].status);
        free(js);
    }
    if (argc > 2) {
        size_t len;
        char *js = slurp(argv[2], &len);
        memset(c, 0, sizeof(*c));
        if (js && devos_cricket_parse_card(js, len, c)) print_card(c);
        else printf("%s: no scorecard\n", argv[2]);
        free(js);
    }
    free(m);
    free(c);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
