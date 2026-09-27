/* devos_cricket: see devos_cricket.h. */
#include "devos_cricket.h"
#include "devos_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef DEVOS_CRICKET_NO_NET          /* host unit test: parsers only */
#define LOCK()
#define UNLOCK()
#else
#include "devos_config.h"
#include "devos_http.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_mx;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void *big_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
}
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void *big_alloc(size_t n) { return calloc(1, n); }
#endif
#endif

#define API_LIST   "https://site.web.api.espn.com/apis/v2/scoreboard/header?sport=cricket&lang=en"
#define API_CARD   "https://site.web.api.espn.com/apis/site/v2/sports/cricket/%s/summary?event=%s&lang=en"
#define LIST_MAX   (768 * 1024)
#define CARD_MAX   (2 * 1024 * 1024)
#define LIVE_MS    30000

/* ------------------------------------------------------------------ dates */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

int devos_cricket_date_add(int ymd, int days)
{
    int64_t z = days_from_civil(ymd / 10000, ymd / 100 % 100, ymd % 100) + days;
    int y, m, d;
    civil_from_days(z, &y, &m, &d);
    return y * 10000 + m * 100 + d;
}

int devos_cricket_today(void)
{
    time_t t = time(NULL);
    if (t < 1700000000) return 0;                   /* clock not set yet */
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* "2026-09-24T05:00:00Z" / "2026-05-31T14:00Z" -> unix time */
static int64_t parse_iso(const char *s)
{
    int y, mo, d, h = 0, mi = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) < 3) return 0;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
}

/* ------------------------------------------------------------------ json helpers */
static const char *mem(const char *obj, const char *end, const char *key) { return devos_json_member(obj, end, key); }

static void mstr(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    if (devos_json_member_str(obj, end, key, out, cap) != 0) out[0] = '\0';
}

static double mnum(const char *obj, const char *end, const char *key, double dflt)
{
    double d;
    return devos_json_member_num(obj, end, key, &d) ? d : dflt;
}

/* true / "true" */
static bool mbool(const char *obj, const char *end, const char *key)
{
    const char *v = mem(obj, end, key);
    return v && (!strncmp(v, "true", 4) || !strncmp(v, "\"true\"", 6));
}

/* ids come as "8039" or 8039 */
static void mid(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    mstr(obj, end, key, out, cap);
    if (!out[0]) {
        double d;
        if (devos_json_member_num(obj, end, key, &d)) snprintf(out, cap, "%.0f", d);
    }
}

/* Calls cb for each element of the array member `key` of obj. */
static void each(const char *obj, const char *end, const char *key, void (*cb)(const char *, size_t, void *), void *ud)
{
    const char *a = mem(obj, end, key);
    if (!a || *a != '[') return;
    const char *ae = devos_json_span(a, end);
    if (ae) devos_json_array_each(a, (size_t)(ae - a), cb, ud);
}

/* First element of the array member `key`, or NULL. */
static const char *first(const char *obj, const char *end, const char *key)
{
    const char *a = mem(obj, end, key);
    if (!a || *a != '[') return NULL;
    const char *p = a + 1;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p < end && *p == '{' ? p : NULL;
}

static devos_cricket_state_t state_of(const char *s)
{
    if (!strcmp(s, "in")) return DEVOS_CRICKET_LIVE;
    if (!strcmp(s, "post")) return DEVOS_CRICKET_DONE;
    return DEVOS_CRICKET_PRE;
}

/* ------------------------------------------------------------------ list */
typedef struct {
    devos_cricket_match_t *out;
    int max, n;
    char league[12], series[64];
    int side;
} list_ud_t;

static void side_one(const char *e, size_t len, void *ud)
{
    list_ud_t *u = ud;
    const char *end = e + len;
    if (u->side >= 2 || *e != '{') return;
    devos_cricket_side_t *s = &u->out[u->n].side[u->side++];
    mstr(e, end, "abbreviation", s->abbr, sizeof(s->abbr));
    mstr(e, end, "displayName", s->name, sizeof(s->name));
    mstr(e, end, "score", s->score, sizeof(s->score));
    s->winner = mbool(e, end, "winner");
}

static void event_one(const char *e, size_t len, void *ud)
{
    list_ud_t *u = ud;
    const char *end = e + len;
    if (u->n >= u->max || *e != '{') return;
    devos_cricket_match_t *m = &u->out[u->n];
    memset(m, 0, sizeof(*m));
    mid(e, end, "id", m->event, sizeof(m->event));
    if (!m->event[0]) return;
    snprintf(m->league, sizeof(m->league), "%s", u->league);
    snprintf(m->series, sizeof(m->series), "%s", u->series);
    mstr(e, end, "title", m->title, sizeof(m->title));
    /* older matches have " at <ground>" as the title: the venue says that */
    const char *t = m->title;
    while (*t == ' ') t++;
    if (!strncmp(t, "at ", 3)) m->title[0] = '\0';
    else if (t != m->title) memmove(m->title, t, strlen(t) + 1);
    mstr(e, end, "location", m->venue, sizeof(m->venue));
    const char *cls = mem(e, end, "class");
    if (cls && *cls == '{') {
        const char *ce = devos_json_span(cls, end);
        if (ce) mstr(cls, ce, "generalClassCard", m->format, sizeof(m->format));
    }
    char st[8] = "", date[32] = "";
    mstr(e, end, "status", st, sizeof(st));
    m->state = state_of(st);
    mstr(e, end, "date", date, sizeof(date));
    m->start = parse_iso(date);
    const char *fs = mem(e, end, "fullStatus");
    if (fs && *fs == '{') {
        const char *fe = devos_json_span(fs, end);
        if (fe) mstr(fs, fe, "longSummary", m->status, sizeof(m->status));
    }
    if (!m->status[0]) mstr(e, end, "summary", m->status, sizeof(m->status));
    u->side = 0;
    each(e, end, "competitors", side_one, u);
    u->n++;
}

static void league_one(const char *l, size_t len, void *ud)
{
    list_ud_t *u = ud;
    const char *end = l + len;
    if (*l != '{') return;
    mid(l, end, "id", u->league, sizeof(u->league));
    mstr(l, end, "name", u->series, sizeof(u->series));
    if (!u->series[0]) mstr(l, end, "shortName", u->series, sizeof(u->series));
    each(l, end, "events", event_one, u);
}

int devos_cricket_parse_list(const char *js, size_t len, devos_cricket_match_t *out, int max)
{
    const char *end = js + len;
    while (js < end && *js != '{') js++;
    const char *sport = first(js, end, "sports");
    if (!sport) return -1;
    const char *se = devos_json_span(sport, end);
    if (!se) return -1;
    list_ud_t u = { .out = out, .max = max };
    each(sport, se, "leagues", league_one, &u);
    return u.n;
}

/* ------------------------------------------------------------------ scorecard */
typedef struct {
    int period;
    /* the stats we use; -1 = absent */
    double batted, pos_bat, runs, balls, fours, sixes, dismissal;
    double bowled, pos_bowl, maidens, conceded, wickets, econ, wides, noballs;
    char overs[8], card[12];
} pstats_t;

static void stat_one(const char *s, size_t len, void *ud)
{
    pstats_t *p = ud;
    const char *end = s + len;
    char name[24];
    mstr(s, end, "name", name, sizeof(name));
    if (!name[0]) return;
    double v = mnum(s, end, "value", 0);
    static const struct { const char *n; size_t off; } F[] = {
        { "batted", offsetof(pstats_t, batted) },           { "battingPosition", offsetof(pstats_t, pos_bat) },
        { "runs", offsetof(pstats_t, runs) },               { "ballsFaced", offsetof(pstats_t, balls) },
        { "fours", offsetof(pstats_t, fours) },             { "sixes", offsetof(pstats_t, sixes) },
        { "dismissal", offsetof(pstats_t, dismissal) },     { "inningsBowled", offsetof(pstats_t, bowled) },
        { "bowlingPosition", offsetof(pstats_t, pos_bowl) }, { "maidens", offsetof(pstats_t, maidens) },
        { "conceded", offsetof(pstats_t, conceded) },       { "wickets", offsetof(pstats_t, wickets) },
        { "economyRate", offsetof(pstats_t, econ) },        { "wides", offsetof(pstats_t, wides) },
        { "noballs", offsetof(pstats_t, noballs) },
    };
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        if (!strcmp(name, F[i].n)) {
            *(double *)((char *)p + F[i].off) = v;
            return;
        }
    }
    if (!strcmp(name, "overs")) mstr(s, end, "displayValue", p->overs, sizeof(p->overs));
    else if (!strcmp(name, "dismissalCard")) mstr(s, end, "displayValue", p->card, sizeof(p->card));
}

static void cat_one(const char *c, size_t len, void *ud) { each(c, c + len, "stats", stat_one, ud); }

typedef struct {
    devos_cricket_card_t *card;
    int team;                       /* index into names */
    char names[2][40];
    char pname[28];
} card_ud_t;

static void add_bat(devos_cricket_innings_t *in, const char *name, const pstats_t *s)
{
    if (in->nbat >= DEVOS_CRICKET_MAX_BAT) return;
    devos_cricket_bat_t *b = &in->bat[in->nbat++];
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->pos = (int)s->pos_bat;
    b->runs = (int)s->runs;
    b->balls = (int)s->balls;
    b->fours = (int)s->fours;
    b->sixes = (int)s->sixes;
    /* "dismissalCard" is "c", "lbw", "run out", "not out" ... but empty for
     * some; "dismissal" is the code (1 c, 2 b, 3 lbw, 4 run out, 5 st, 6 hw, 12 not out) */
    static const char *const CODE[] = { "", "c", "b", "lbw", "run out", "st", "hit wkt" };
    int code = (int)s->dismissal;
    if (s->card[0]) snprintf(b->how, sizeof(b->how), "%s", s->card);
    else if (code > 0 && code < (int)(sizeof(CODE) / sizeof(CODE[0]))) snprintf(b->how, sizeof(b->how), "%s", CODE[code]);
    else snprintf(b->how, sizeof(b->how), "%s", code == 12 || code == 0 ? "not out" : "out");
}

static void add_bowl(devos_cricket_innings_t *in, const char *name, const pstats_t *s)
{
    if (in->nbowl >= DEVOS_CRICKET_MAX_BOWL) return;
    devos_cricket_bowl_t *w = &in->bowl[in->nbowl++];
    snprintf(w->name, sizeof(w->name), "%s", name);
    snprintf(w->overs, sizeof(w->overs), "%s", s->overs[0] ? s->overs : "0");
    w->pos = (int)s->pos_bowl;
    w->maidens = (int)s->maidens;
    w->runs = (int)s->conceded;
    w->wickets = (int)s->wickets;
    w->wides = (int)s->wides;
    w->noballs = (int)s->noballs;
    w->econ = (float)s->econ;
}

/* One entry of a player's "linescores": an innings of the match. */
static void pline_one(const char *l, size_t len, void *ud)
{
    card_ud_t *u = ud;
    const char *end = l + len;
    int period = (int)mnum(l, end, "period", 0);
    if (period < 1 || period > DEVOS_CRICKET_MAX_INN) return;
    const char *inner = first(l, end, "linescores");
    if (!inner) return;
    const char *ie = devos_json_span(inner, end);
    const char *st = ie ? mem(inner, ie, "statistics") : NULL;
    if (!st || *st != '{') return;
    const char *ste = devos_json_span(st, ie);
    if (!ste) return;
    pstats_t s = { .period = period };
    each(st, ste, "categories", cat_one, &s);
    devos_cricket_innings_t *in = &u->card->inn[period - 1];
    if (s.batted > 0) {
        if (!in->team[0]) snprintf(in->team, sizeof(in->team), "%s", u->names[u->team]);
        add_bat(in, u->pname, &s);
        if (period > u->card->ninn) u->card->ninn = period;
    }
    if (s.bowled > 0) add_bowl(in, u->pname, &s);
}

static void player_one(const char *p, size_t len, void *ud)
{
    card_ud_t *u = ud;
    const char *end = p + len;
    const char *a = mem(p, end, "athlete");
    u->pname[0] = '\0';
    if (a && *a == '{') {
        const char *ae = devos_json_span(a, end);
        if (ae) {
            mstr(a, ae, "battingName", u->pname, sizeof(u->pname));
            if (!u->pname[0]) mstr(a, ae, "displayName", u->pname, sizeof(u->pname));
        }
    }
    if (u->pname[0]) each(p, end, "linescores", pline_one, u);
}

static void roster_one(const char *r, size_t len, void *ud)
{
    card_ud_t *u = ud;
    const char *end = r + len;
    if (u->team >= 1 && u->names[1][0]) return;
    const char *t = mem(r, end, "team");
    int idx = u->names[0][0] ? 1 : 0;
    if (t && *t == '{') {
        const char *te = devos_json_span(t, end);
        if (te) mstr(t, te, "displayName", u->names[idx], sizeof(u->names[idx]));
    }
    u->team = idx;
    each(r, end, "roster", player_one, u);
}

/* Innings totals from header.competitions[0].competitors[].linescores[]. */
typedef struct {
    devos_cricket_card_t *card;
    char team[40];
} tot_ud_t;

static void tot_line(const char *l, size_t len, void *ud)
{
    tot_ud_t *u = ud;
    const char *end = l + len;
    int period = (int)mnum(l, end, "period", 0);
    if (period < 1 || period > DEVOS_CRICKET_MAX_INN) return;
    devos_cricket_innings_t *in = &u->card->inn[period - 1];
    if (strcmp(in->team, u->team)) return;          /* this side didn't bat in that innings */
    char score[40] = "", desc[24] = "";
    mstr(l, end, "score", score, sizeof(score));
    mstr(l, end, "description", desc, sizeof(desc));
    if (!score[0]) {
        snprintf(score, sizeof(score), "%d/%d (%g ov)", (int)mnum(l, end, "runs", 0), (int)mnum(l, end, "wickets", 0),
                 mnum(l, end, "overs", 0));
    }
    bool extra = desc[0] && strcmp(desc, "complete") && !strstr(score, desc);
    snprintf(in->total, sizeof(in->total), "%.30s%s%.16s", score, extra ? " " : "", extra ? desc : "");
}

static void tot_comp(const char *c, size_t len, void *ud)
{
    tot_ud_t *u = ud;
    const char *end = c + len;
    const char *t = mem(c, end, "team");
    u->team[0] = '\0';
    if (t && *t == '{') {
        const char *te = devos_json_span(t, end);
        if (te) mstr(t, te, "displayName", u->team, sizeof(u->team));
    }
    each(c, end, "linescores", tot_line, u);
}

static void note_one(const char *n, size_t len, void *ud)
{
    devos_cricket_card_t *c = ud;
    const char *end = n + len;
    char type[16];
    mstr(n, end, "type", type, sizeof(type));
    if (!strcmp(type, "toss") && !c->toss[0]) mstr(n, end, "text", c->toss, sizeof(c->toss));
}

static int cmp_bat(const void *a, const void *b) { return ((const devos_cricket_bat_t *)a)->pos - ((const devos_cricket_bat_t *)b)->pos; }
static int cmp_bowl(const void *a, const void *b) { return ((const devos_cricket_bowl_t *)a)->pos - ((const devos_cricket_bowl_t *)b)->pos; }

bool devos_cricket_parse_card(const char *js, size_t len, devos_cricket_card_t *c)
{
    const char *end = js + len;
    while (js < end && *js != '{') js++;
    if (js >= end) return false;
    const char *h = mem(js, end, "header");
    if (!h || *h != '{') return false;
    const char *he = devos_json_span(h, end);
    if (!he) return false;
    char name[64] = "", desc[48] = "";
    mstr(h, he, "name", name, sizeof(name));
    const char *comp = first(h, he, "competitions");
    const char *ce = comp ? devos_json_span(comp, he) : NULL;
    if (ce) {
        mstr(comp, ce, "description", desc, sizeof(desc));
        const char *st = mem(comp, ce, "status");
        const char *ste = st && *st == '{' ? devos_json_span(st, ce) : NULL;
        if (ste) {
            mstr(st, ste, "summary", c->result, sizeof(c->result));
            const char *ty = mem(st, ste, "type");
            const char *tye = ty && *ty == '{' ? devos_json_span(ty, ste) : NULL;
            if (tye) {
                char s[8];
                mstr(ty, tye, "state", s, sizeof(s));
                c->live = state_of(s) == DEVOS_CRICKET_LIVE;
            }
        }
    }
    snprintf(c->title, sizeof(c->title), "%.60s%s%.32s", name, desc[0] ? " - " : "", desc);
    const char *gi = mem(js, end, "gameInfo");
    const char *gie = gi && *gi == '{' ? devos_json_span(gi, end) : NULL;
    const char *v = gie ? mem(gi, gie, "venue") : NULL;
    const char *ve = v && *v == '{' ? devos_json_span(v, gie) : NULL;
    if (ve) mstr(v, ve, "fullName", c->venue, sizeof(c->venue));
    each(js, end, "notes", note_one, c);

    card_ud_t u = { .card = c };
    each(js, end, "rosters", roster_one, &u);
    if (ce) {
        tot_ud_t t = { .card = c };
        each(comp, ce, "competitors", tot_comp, &t);
    }
    for (int i = 0; i < c->ninn; i++) {
        qsort(c->inn[i].bat, (size_t)c->inn[i].nbat, sizeof(devos_cricket_bat_t), cmp_bat);
        qsort(c->inn[i].bowl, (size_t)c->inn[i].nbowl, sizeof(devos_cricket_bowl_t), cmp_bowl);
    }
    c->loaded = true;
    return true;
}

#ifndef DEVOS_CRICKET_NO_NET
/* ESPN's edge answers 502 now and then, mostly the first time an old date is
 * asked for; a second try a moment later works. */
static void get(devos_http_req_t *q, devos_http_resp_t *r)
{
    for (int attempt = 0;; attempt++) {
        devos_http_request(q, r);
        if (r->status < 500 || attempt == 2) return;
        devos_http_resp_free(r);
        sleep_ms(1500);
    }
}

/* ------------------------------------------------------------------ state */
static devos_cricket_match_t *s_list, *s_tmp;   /* DEVOS_CRICKET_MAX_MATCHES, PSRAM */
static int s_n;
static devos_cricket_status_t s_st;
static devos_cricket_card_t *s_card, *s_card_tmp;
static char s_want_league[12], s_want_event[12];
static bool s_list_dirty = true, s_card_dirty, s_inited, s_worker;
static volatile bool s_active;
static int64_t s_list_at, s_card_at;
static uint32_t s_gen;

static void fetch_list(void)
{
    char url[160];
    LOCK();
    int date = s_st.date;
    s_st.loading = true;
    s_list_dirty = false;
    s_gen++;
    UNLOCK();
    if (date) snprintf(url, sizeof(url), API_LIST "&dates=%08d", date);
    else snprintf(url, sizeof(url), API_LIST);
    devos_http_req_t q = { .url = url, .timeout_ms = 15000, .max_body = LIST_MAX, .max_redirects = 2 };
    devos_http_resp_t r;
    get(&q, &r);
    int n = -1;
    if (r.status == 200 && r.body) n = devos_cricket_parse_list(r.body, r.body_len, s_tmp, DEVOS_CRICKET_MAX_MATCHES);
    LOCK();
    s_st.loading = false;
    if (s_st.date != date) {                        /* the user moved on meanwhile */
        UNLOCK();
        devos_http_resp_free(&r);
        return;
    }
    if (n >= 0) {
        memcpy(s_list, s_tmp, sizeof(*s_list) * (size_t)n);
        s_n = n;
        s_st.count = n;
        s_st.live = 0;
        for (int i = 0; i < n; i++) s_st.live += s_list[i].state == DEVOS_CRICKET_LIVE;
        s_st.updated = time(NULL);
        s_st.error[0] = '\0';
    } else if (!r.status) {
        snprintf(s_st.error, sizeof(s_st.error), "%.95s", r.error);
    } else {
        snprintf(s_st.error, sizeof(s_st.error), "ESPNcricinfo: HTTP %d", r.status);
    }
    s_list_at = now_ms();
    s_gen++;
    UNLOCK();
    devos_http_resp_free(&r);
}

static void fetch_card(void)
{
    char league[12], event[12], url[200];
    LOCK();
    snprintf(league, sizeof(league), "%s", s_want_league);
    snprintf(event, sizeof(event), "%s", s_want_event);
    s_card_dirty = false;
    s_card->loading = true;
    s_gen++;
    UNLOCK();
    snprintf(url, sizeof(url), API_CARD, league, event);
    devos_http_req_t q = { .url = url, .timeout_ms = 20000, .max_body = CARD_MAX, .max_redirects = 2 };
    devos_http_resp_t r;
    get(&q, &r);
    memset(s_card_tmp, 0, sizeof(*s_card_tmp));
    bool ok = r.status == 200 && r.body && devos_cricket_parse_card(r.body, r.body_len, s_card_tmp);
    LOCK();
    if (strcmp(event, s_want_event)) {              /* another match was opened meanwhile */
        UNLOCK();
        devos_http_resp_free(&r);
        return;
    }
    if (ok) {
        *s_card = *s_card_tmp;
        s_card->updated = time(NULL);
    } else {
        s_card->loaded = s_card->loaded && !strcmp(s_card->event, event);
        if (!r.status) snprintf(s_card->error, sizeof(s_card->error), "%.95s", r.error);
        else snprintf(s_card->error, sizeof(s_card->error), "Scorecard: HTTP %d", r.status);
    }
    snprintf(s_card->event, sizeof(s_card->event), "%s", event);
    snprintf(s_card->league, sizeof(s_card->league), "%s", league);
    s_card->loading = false;
    s_card_at = now_ms();
    s_gen++;
    UNLOCK();
    devos_http_resp_free(&r);
}

static void worker_loop(void)
{
    for (;;) {
        if (s_active) {
            LOCK();
            int64_t t = now_ms();
            bool list = s_list_dirty || (s_st.date == 0 && t - s_list_at > LIVE_MS);
            bool card = s_want_event[0] &&
                        (s_card_dirty || (s_card->live && s_st.date == 0 && t - s_card_at > LIVE_MS));
            UNLOCK();
            if (list) fetch_list();
            if (card) fetch_card();
        }
        sleep_ms(200);
    }
}

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    (void)arg;
    worker_loop();
}
#else
static void *worker_thread(void *arg)
{
    (void)arg;
    worker_loop();
    return NULL;
}
#endif

/* ------------------------------------------------------------------ api */
void devos_cricket_init(void)
{
    if (s_inited) return;
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
#endif
    s_list = big_alloc(sizeof(*s_list) * DEVOS_CRICKET_MAX_MATCHES);
    s_tmp = big_alloc(sizeof(*s_tmp) * DEVOS_CRICKET_MAX_MATCHES);
    s_card = big_alloc(sizeof(*s_card));
    s_card_tmp = big_alloc(sizeof(*s_card_tmp));
    s_inited = s_list && s_tmp && s_card && s_card_tmp;
}

void devos_cricket_set_active(bool active)
{
    if (!s_inited) return;
    s_active = active;
    LOCK();
    s_st.active = active;
    bool start = active && !s_worker;
    s_worker = s_worker || start;
    UNLOCK();
    if (!start) return;
#ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(worker_task, "cricket", 12288, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO);
#else
    pthread_t th;
    pthread_create(&th, NULL, worker_thread, NULL);
    pthread_detach(th);
#endif
}

void devos_cricket_set_date(int ymd)
{
    if (!s_inited) return;
    LOCK();
    if (ymd == devos_cricket_today()) ymd = 0;
    if (ymd != s_st.date) {
        s_st.date = ymd;
        s_n = 0;
        s_st.count = s_st.live = 0;
        s_st.error[0] = '\0';
        s_list_dirty = true;
        s_gen++;
    }
    UNLOCK();
}

void devos_cricket_refresh(void)
{
    if (!s_inited) return;
    LOCK();
    s_list_dirty = true;
    if (s_want_event[0]) s_card_dirty = true;
    UNLOCK();
}

void devos_cricket_status(devos_cricket_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_inited) {
        snprintf(out->error, sizeof(out->error), "off");
        return;
    }
    LOCK();
    *out = s_st;
    UNLOCK();
    out->clock_ok = devos_cricket_today() != 0;
}

int devos_cricket_matches(devos_cricket_match_t *out, int max)
{
    if (!s_inited) return 0;
    LOCK();
    int n = s_n < max ? s_n : max;
    if (n > 0) memcpy(out, s_list, sizeof(*out) * (size_t)n);
    UNLOCK();
    return n;
}

void devos_cricket_open(const char *league, const char *event)
{
    if (!s_inited) return;
    LOCK();
    if (!event || !league) {
        s_want_event[0] = s_want_league[0] = '\0';
    } else if (strcmp(event, s_want_event)) {
        snprintf(s_want_league, sizeof(s_want_league), "%s", league);
        snprintf(s_want_event, sizeof(s_want_event), "%s", event);
        memset(s_card, 0, sizeof(*s_card));
        snprintf(s_card->event, sizeof(s_card->event), "%s", event);
        s_card->loading = true;
        s_card_dirty = true;
    }
    s_gen++;
    UNLOCK();
}

void devos_cricket_card(devos_cricket_card_t *out)
{
    if (!s_inited) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK();
    *out = *s_card;
    UNLOCK();
}

uint32_t devos_cricket_generation(void)
{
    LOCK();
    uint32_t g = s_gen;
    UNLOCK();
    return g;
}
#endif /* DEVOS_CRICKET_NO_NET */
