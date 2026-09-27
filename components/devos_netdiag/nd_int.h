#pragma once
/* devos_netdiag internals: platform glue shared by the engines. */
#include "devos_netdiag.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
typedef SemaphoreHandle_t nd_mutex_t;
#define ND_MUTEX_INIT NULL
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t nd_mutex_t;
#define ND_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#endif

void nd_mutex_create(nd_mutex_t *m);
void nd_lock(nd_mutex_t *m);
void nd_unlock(nd_mutex_t *m);
int64_t nd_ms(void);
void nd_sleep_ms(int ms);
/* Run fn(arg) on a new Core 0 task / thread that ends with fn. */
int nd_spawn(void (*fn)(void *), void *arg, const char *name, int stack);
void *nd_alloc(size_t n);               /* PSRAM when there is some */
/* Block until readable (or timeout); 1 readable, 0 timeout, -1 error. */
int nd_wait_readable(int fd, int timeout_ms);

/* ping.c: ICMP echo helpers shared with the port scanner's host sweep */
int nd_icmp_socket(void);
int nd_icmp_send(int sock, uint32_t ip, uint16_t id, uint16_t seq, int size);
/* One reply: fills ip / seq / ttl; returns 1, 0 on timeout, -1 on error. */
int nd_icmp_recv(int sock, int timeout_ms, uint32_t *ip, uint16_t *seq, uint16_t *id, int *ttl);

/* dns.c: build / parse helpers shared with mDNS */
int nd_dns_build_query(uint8_t *buf, int cap, uint16_t id, const char *const *names, const uint16_t *types, int n,
                       bool rd, bool qu);
/* Read a (compressed) name at *off into out ("a.b.c", no trailing dot). */
int nd_dns_read_name(const uint8_t *msg, int len, int *off, char *out, size_t cap);
