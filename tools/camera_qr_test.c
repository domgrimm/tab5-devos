/* Unit tests for Tab5 camera & QR subsystem */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "bsp_tab5_camera.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static char s_last_detected[256] = "";
static int s_callback_count = 0;

static void test_qr_cb(const char *qr_payload, void *user_data)
{
    int *call_count = (int *)user_data;
    if (call_count) (*call_count)++;
    s_callback_count++;
    strncpy(s_last_detected, qr_payload, sizeof(s_last_detected) - 1);
    s_last_detected[sizeof(s_last_detected) - 1] = '\0';
}

int main(void)
{
    printf("running camera_qr_test...\n");

    /* 1. Init camera */
    CHECK(bsp_tab5_camera_init() == true);

    /* 2. Start camera & verify frame buffer */
    CHECK(bsp_tab5_camera_start() == true);
    int width = 0, height = 0;
    const uint8_t *frame = bsp_tab5_camera_get_frame(&width, &height);
    CHECK(frame != NULL);
    CHECK(width == 320);
    CHECK(height == 240);

    /* Verify frame has valid grayscale test pattern data */
    int non_zero = 0;
    for (int i = 0; i < width * height; i++) {
        if (frame[i] > 0) non_zero++;
    }
    CHECK(non_zero > 100);

    /* 3. Start QR scanner */
    int user_counter = 0;
    CHECK(bsp_tab5_camera_start_qr_scanner(test_qr_cb, &user_counter) == true);

    /* 4. Inject OpenChamber QR payload and test callback invocation */
    const char *test_uri = "openchamber://connect?host=100.77.11.92&port=8421&token=sec_camera_scanned";
    bsp_tab5_camera_inject_qr(test_uri);

    CHECK(s_callback_count == 1);
    CHECK(user_counter == 1);
    CHECK(strcmp(s_last_detected, test_uri) == 0);

    /* 5. Scanner should have auto-stopped after injection/detection */
    s_callback_count = 0;
    bsp_tab5_camera_inject_qr(test_uri);
    CHECK(s_callback_count == 0); /* should not trigger since scanner stopped */

    /* 6. Stop camera */
    bsp_tab5_camera_stop();

    if (failures == 0) {
        printf("camera_qr unit tests: ALL PASS\n");
        return 0;
    } else {
        printf("camera_qr unit tests: %d FAILURES\n", failures);
        return 1;
    }
}
