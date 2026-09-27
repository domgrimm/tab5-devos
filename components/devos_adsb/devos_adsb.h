#pragma once

/* devos_adsb: live aircraft from a local ADS-B receiver (no LVGL) - the
 * aircraft.json that dump1090-fa, readsb, tar1090 and PiAware's SkyAware
 * serve (the same data they feed to Flightradar24 / FlightAware).
 *
 * A Core 0 worker polls it once a second while the app is open, keeps a
 * short position trail per aircraft and drops the ones not heard for a
 * minute. The receiver position comes from the settings or, if blank, from
 * the feeder's receiver.json next to aircraft.json.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_ADSB_MAX   256
#define DEVOS_ADSB_TRAIL 48

typedef struct {
    char url[200];                  /* .../data/aircraft.json */
    double lat, lon;                /* receiver; 0,0 = ask receiver.json */
    int range_nm;                   /* initial radar range */
} devos_adsb_config_t;

typedef struct {
    char hex[8];
    char flight[10];                /* callsign, trimmed ("" if none) */
    char squawk[6];
    char category[4];               /* A0..D7 */
    bool ground;
    bool has_pos, has_alt, has_track, has_gs;
    int alt_ft;                     /* barometric (geometric if that's all there is) */
    int alt_geom_ft;
    int vrate_fpm;
    float gs_kt, track_deg;
    double lat, lon;
    float seen_s, seen_pos_s;
    float rssi;
    uint32_t messages;
    bool emergency;                 /* squawk 7500/7600/7700 or an emergency field */
    bool mlat;
    /* trail, oldest first */
    int trail_n;
    float trail_lat[DEVOS_ADSB_TRAIL], trail_lon[DEVOS_ADSB_TRAIL];
} devos_adsb_ac_t;

typedef struct {
    bool configured, active;
    double lat, lon;                /* receiver position in use */
    bool have_pos;
    int total, with_pos;
    float msg_rate;                 /* messages / s */
    float max_range_nm;             /* furthest position seen this session */
    int64_t updated;                /* time() of the last good fetch */
    char error[112];
} devos_adsb_status_t;

void devos_adsb_init(void);
void devos_adsb_get_config(devos_adsb_config_t *out);
void devos_adsb_set_config(const devos_adsb_config_t *c);
bool devos_adsb_configured(void);
void devos_adsb_set_active(bool active);
void devos_adsb_status(devos_adsb_status_t *out);
/* Copies the aircraft (unsorted); returns the count. */
int devos_adsb_list(devos_adsb_ac_t *out, int max);
uint32_t devos_adsb_generation(void);

/* Distance (nm) and bearing (degrees true) from the receiver. */
void devos_adsb_range_bearing(double lat0, double lon0, double lat, double lon, float *nm, float *brg);
/* "Heavy jet", "Helicopter" ... for an emitter category, or NULL. */
const char *devos_adsb_category_name(const char *cat);

#ifdef __cplusplus
}
#endif
