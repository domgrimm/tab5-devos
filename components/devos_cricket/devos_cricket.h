#pragma once

/* devos_cricket: live scores, results and scorecards from ESPNcricinfo
 * (no LVGL).
 *
 * Data comes from ESPN's public site API, which serves ESPNcricinfo's
 * database (the match ids are Cricinfo's):
 *   match list  site.web.api.espn.com/apis/v2/scoreboard/header?sport=cricket
 *               [&dates=YYYYMMDD] - every match on that day, any year
 *   scorecard   .../apis/site/v2/sports/cricket/<series>/summary?event=<match>
 *
 * A Core 0 worker fetches while the app is open: today's list every 30 s,
 * another day's once, and the open scorecard every 30 s while it's live.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_CRICKET_MAX_MATCHES 96
#define DEVOS_CRICKET_MAX_INN     4
#define DEVOS_CRICKET_MAX_BAT     12
#define DEVOS_CRICKET_MAX_BOWL    11

typedef enum {
    DEVOS_CRICKET_PRE = 0,          /* not started */
    DEVOS_CRICKET_LIVE,
    DEVOS_CRICKET_DONE,             /* result, draw, abandoned */
} devos_cricket_state_t;

typedef struct {
    char abbr[8];
    char name[40];
    char score[48];                 /* "241 & 290/8d", "174/2 (31.4/50 ov)" */
    bool winner;
} devos_cricket_side_t;

typedef struct {
    char event[12];                 /* match id */
    char league[12];                /* series id */
    char series[64];
    char title[48];                 /* "3rd ODI", "Final" */
    char format[16];                /* "Test", "ODI", "T20I", "First-class" ... */
    char venue[48];
    char status[112];               /* "England won by 5 wickets", "Lancashire trail by 197 runs" */
    int64_t start;                  /* unix time */
    devos_cricket_state_t state;
    devos_cricket_side_t side[2];
} devos_cricket_match_t;

typedef struct {
    bool active, loading, clock_ok;
    int date;                       /* YYYYMMDD shown, 0 = today (live) */
    int count, live;
    int64_t updated;                /* time() of the last good fetch */
    char error[96];
} devos_cricket_status_t;

typedef struct {
    char name[28];
    char how[12];                   /* "not out", "c", "lbw", "run out", "out" */
    int pos, runs, balls, fours, sixes;
} devos_cricket_bat_t;

typedef struct {
    char name[28];
    char overs[8];
    int pos, maidens, runs, wickets, wides, noballs;
    float econ;
} devos_cricket_bowl_t;

typedef struct {
    char team[40];                  /* batting side */
    char total[48];                 /* "241 all out (50 ov)" */
    int nbat, nbowl;
    devos_cricket_bat_t bat[DEVOS_CRICKET_MAX_BAT];
    devos_cricket_bowl_t bowl[DEVOS_CRICKET_MAX_BOWL];
} devos_cricket_innings_t;

typedef struct {
    char event[12], league[12];
    bool loaded, loading;
    char error[96];
    char title[96];                 /* "England v New Zealand - Final" */
    char venue[64];
    char toss[96];
    char result[112];
    bool live;
    int ninn;
    devos_cricket_innings_t inn[DEVOS_CRICKET_MAX_INN];
    int64_t updated;
} devos_cricket_card_t;

void devos_cricket_init(void);
void devos_cricket_set_active(bool active);
/* Day to list (YYYYMMDD, 0 = today's live view). */
void devos_cricket_set_date(int yyyymmdd);
void devos_cricket_refresh(void);
void devos_cricket_status(devos_cricket_status_t *out);
/* Copies the match list; returns the count. */
int devos_cricket_matches(devos_cricket_match_t *out, int max);
/* Load a match's scorecard (NULL closes it). */
void devos_cricket_open(const char *league, const char *event);
void devos_cricket_card(devos_cricket_card_t *out);
uint32_t devos_cricket_generation(void);

/* Today's local date as YYYYMMDD, or 0 while the clock isn't set. */
int devos_cricket_today(void);
/* YYYYMMDD plus `days` (may be negative). */
int devos_cricket_date_add(int yyyymmdd, int days);

/* The parsers, exposed for tools/cricket_test.c. */
int devos_cricket_parse_list(const char *js, size_t len, devos_cricket_match_t *out, int max);
bool devos_cricket_parse_card(const char *js, size_t len, devos_cricket_card_t *out);

#ifdef __cplusplus
}
#endif
