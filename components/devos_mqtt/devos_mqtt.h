#pragma once

/* devos_mqtt: MQTT 3.1.1 client engine for the MQTT app (no LVGL).
 *
 * One worker (FreeRTOS task on the network core; a pthread in the simulator)
 * owns the socket: connect, subscribe, read, keepalive, the publish queue and
 * reconnects. Received messages go into a ring buffer (PSRAM on the device,
 * allocated on first start) that the UI reads by sequence number, plus a
 * sorted table of the topics seen. Plain TCP only (no TLS).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_MQTT_MAX_SUBS     4
#define DEVOS_MQTT_TOPIC_MAX    192
#define DEVOS_MQTT_PAYLOAD_MAX  4096    /* stored per message; longer payloads are cut */
#define DEVOS_MQTT_RING         500     /* messages kept */
#define DEVOS_MQTT_MAX_TOPICS   256

typedef struct {
    char host[96];
    int port;                           /* 1883 */
    char client_id[48];                 /* "" = devos-tab5-xxxx */
    char username[64];
    char password[96];
    char subs[DEVOS_MQTT_MAX_SUBS][DEVOS_MQTT_TOPIC_MAX];   /* "" = unused; default "#" */
    int keepalive_s;                    /* 30 */
} devos_mqtt_config_t;

typedef enum {
    DEVOS_MQTT_IDLE = 0,                /* not started (or stopped after an error) */
    DEVOS_MQTT_CONNECTING,
    DEVOS_MQTT_UP,
    DEVOS_MQTT_RETRYING,                /* lost / failed; reconnecting soon */
} devos_mqtt_state_t;

typedef struct {
    uint32_t seq;
    uint32_t time_s;                    /* wall clock when received */
    uint32_t len;                       /* full payload length */
    uint16_t stored;                    /* bytes kept (<= DEVOS_MQTT_PAYLOAD_MAX) */
    uint8_t qos;
    bool retain;
    char topic[DEVOS_MQTT_TOPIC_MAX];
} devos_mqtt_msg_t;

typedef struct {
    char topic[DEVOS_MQTT_TOPIC_MAX];
    uint32_t count;
    uint32_t last_seq;
    uint32_t last_time_s;
} devos_mqtt_topic_t;

void devos_mqtt_init(void);

/* Broker settings: SD card JSON (password in NVS on the device). Setting
 * them reconnects if running. */
void devos_mqtt_get_config(devos_mqtt_config_t *out);
void devos_mqtt_set_config(const devos_mqtt_config_t *cfg);
bool devos_mqtt_configured(void);       /* a host has been set */

int devos_mqtt_start(void);             /* connect and keep reconnecting */
void devos_mqtt_stop(void);
devos_mqtt_state_t devos_mqtt_state(void);
void devos_mqtt_status_text(char *out, size_t cap);

/* Queue a publish (0 = queued, -1 = not connected / queue full). */
int devos_mqtt_publish(const char *topic, const char *payload, size_t len, int qos, bool retain);

/* Messages: [seq_first, seq_next) are available. */
uint32_t devos_mqtt_generation(void);   /* bumps on new messages and clear */
uint32_t devos_mqtt_seq_first(void);
uint32_t devos_mqtt_seq_next(void);
/* Copy message seq (payload NUL-terminated, cut to cap). False if it's gone. */
bool devos_mqtt_get(uint32_t seq, devos_mqtt_msg_t *meta, char *payload, size_t cap);
void devos_mqtt_clear(void);

/* Topics seen, sorted by name. */
int devos_mqtt_topic_count(void);
bool devos_mqtt_topic(int idx, devos_mqtt_topic_t *out);

/* Totals since start/clear and messages per second over the last 5 s. */
void devos_mqtt_stats(uint32_t *messages, uint32_t *bytes, float *per_s);

#ifdef __cplusplus
}
#endif
