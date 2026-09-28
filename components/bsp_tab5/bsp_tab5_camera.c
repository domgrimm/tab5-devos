/* bsp_tab5_camera: see bsp_tab5_camera.h. */
#include "bsp_tab5_camera.h"
#include "bsp_tab5.h"
#include "devos_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_err[168];

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "camera";

static esp_cam_sensor_device_t *s_cam;
static esp_cam_ctlr_handle_t s_csi;
static isp_proc_handle_t s_isp;
static uint8_t *s_fb[2];                /* RGB565 frames the CSI DMA fills */
static size_t s_fb_len;
static int s_w, s_h;
static bool s_ready_hw, s_streaming;
static SemaphoreHandle_t s_frame_sem;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_filling = -1, s_done = -1, s_held = -1;

/* software auto-exposure */
static int32_t s_exp, s_exp_min, s_exp_max;
static int32_t s_gain, s_gain_max;
static int s_ae_skip;

/* The DMA wants a buffer for the next frame: one the reader isn't holding
 * and that isn't the unread latest frame. None free: the driver drops the
 * frame into its own backup buffer. */
static bool IRAM_ATTR on_new_trans(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h; (void)ud;
    portENTER_CRITICAL_ISR(&s_mux);
    int pick = -1;
    for (int i = 0; i < 2; i++) {
        if (i != s_held && i != s_done) { pick = i; break; }
    }
    s_filling = pick;
    portEXIT_CRITICAL_ISR(&s_mux);
    if (pick >= 0) {
        trans->buffer = s_fb[pick];
        trans->buflen = s_fb_len;
    }
    return false;
}

static bool IRAM_ATTR on_trans_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h; (void)ud;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_mux);
    for (int i = 0; i < 2; i++) {
        if (trans->buffer == s_fb[i]) s_done = i;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

/* The drivers say why in their own error log line ("intr_alloc: No free
 * interrupt inputs ...") - keep the last one printed while the camera is
 * being set up, so the message on screen names the real cause. Only for that
 * short window: the hook formats on the logging task's stack. */
static vprintf_like_t s_prev_log;
static char s_idf_err[112];

static int log_hook(const char *fmt, va_list ap)
{
    const char *p = fmt;
    if (*p == '\033' && (p = strchr(p, 'm')) != NULL) p++;           /* colour code */
    if (p && p[0] == 'E' && p[1] == ' ' && p[2] == '(') {
        va_list cp;
        va_copy(cp, ap);
        char line[160];
        vsnprintf(line, sizeof(line), fmt, cp);
        va_end(cp);
        const char *m = strstr(line, ") ");                             /* past "E (1234) " */
        m = m ? m + 2 : line;
        size_t n = strcspn(m, "\033\r\n");
        if (n >= sizeof(s_idf_err)) n = sizeof(s_idf_err) - 1;
        memcpy(s_idf_err, m, n);
        s_idf_err[n] = '\0';
    }
    return s_prev_log ? s_prev_log(fmt, ap) : vprintf(fmt, ap);
}

static void fail(const char *what, esp_err_t e)
{
    if (s_idf_err[0]) snprintf(s_err, sizeof(s_err), "%s (%s): %.60s", what, esp_err_to_name(e), s_idf_err);
    else snprintf(s_err, sizeof(s_err), "%s (%s)", what, esp_err_to_name(e));
    ESP_LOGE(TAG, "%s", s_err);
}

static color_raw_element_order_t bayer_order(esp_cam_sensor_bayer_pattern_t b)
{
    switch (b) {
    case ESP_CAM_SENSOR_BAYER_RGGB: return COLOR_RAW_ELEMENT_ORDER_RGGB;
    case ESP_CAM_SENSOR_BAYER_GRBG: return COLOR_RAW_ELEMENT_ORDER_GRBG;
    case ESP_CAM_SENSOR_BAYER_GBRG: return COLOR_RAW_ELEMENT_ORDER_GBRG;
    default:                        return COLOR_RAW_ELEMENT_ORDER_BGGR;
    }
}

static void ae_init(void)
{
    esp_cam_sensor_param_desc_t d = { .id = ESP_CAM_SENSOR_EXPOSURE_VAL };
    if (esp_cam_sensor_query_para_desc(s_cam, &d) == ESP_OK) {
        s_exp_min = d.number.minimum;
        s_exp_max = d.number.maximum;
        s_exp = d.default_value;
    }
    d = (esp_cam_sensor_param_desc_t){ .id = ESP_CAM_SENSOR_GAIN };
    if (esp_cam_sensor_query_para_desc(s_cam, &d) == ESP_OK) {
        s_gain_max = (int32_t)d.enumeration.count - 1;
        s_gain = d.default_value;
    }
}

/* Nudge exposure / gain towards a mid-grey average (QR codes want contrast,
 * not colour accuracy). Runs every few frames so the sensor can settle. */
static void ae_step(uint32_t mean)
{
    if (!s_exp_max || ++s_ae_skip < 3) return;
    s_ae_skip = 0;
    int32_t exp = s_exp, gain = s_gain;
    if (mean < 85) {
        if (exp < s_exp_max) exp = exp + exp / 4 + 16 > s_exp_max ? s_exp_max : exp + exp / 4 + 16;
        else if (gain < s_gain_max) gain++;
    } else if (mean > 170) {
        if (gain > 0) gain--;
        else if (exp > s_exp_min) exp = exp - exp / 4 < s_exp_min ? s_exp_min : exp - exp / 4;
    } else {
        return;
    }
    if (exp != s_exp) {
        uint32_t v = (uint32_t)exp;
        if (esp_cam_sensor_set_para_value(s_cam, ESP_CAM_SENSOR_EXPOSURE_VAL, &v, sizeof(v)) == ESP_OK) s_exp = exp;
    }
    if (gain != s_gain) {
        uint32_t v = (uint32_t)gain;
        if (esp_cam_sensor_set_para_value(s_cam, ESP_CAM_SENSOR_GAIN, &v, sizeof(v)) == ESP_OK) s_gain = gain;
    }
}

static bool hw_init(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bsp_tab5_i2c_bus_internal();
    if (!bus) {
        snprintf(s_err, sizeof(s_err), "I2C bus not ready");
        return false;
    }
    /* find the sensor (only SC202CS is enabled in sdkconfig) */
    esp_cam_sensor_config_t cfg = {
        .reset_pin = -1,                /* held high by the IO expander */
        .pwdn_pin = -1,
        .xclk_pin = -1,                 /* 24 MHz oscillator on the board */
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    /* each step is skipped if an earlier (failed) start already did it */
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end && !s_cam; ++p) {
        sccb_i2c_config_t ic = {
            .scl_speed_hz = 400000,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        if (sccb_new_i2c_io(bus, &ic, &cfg.sccb_handle) != ESP_OK) continue;
        s_cam = (*(p->detect))(&cfg);
        if (!s_cam) esp_sccb_del_i2c_io(cfg.sccb_handle);
    }
    if (!s_cam) {
        snprintf(s_err, sizeof(s_err), "No camera found");
        return false;
    }
    esp_cam_sensor_format_array_t fa = {0};
    esp_cam_sensor_query_format(s_cam, &fa);
    const esp_cam_sensor_format_t *fmt = NULL;
    for (uint32_t i = 0; i < fa.count; i++) {
        if (strstr(fa.format_array[i].name, "RAW8_1280x720")) fmt = &fa.format_array[i];
    }
    if (!fmt && fa.count) fmt = &fa.format_array[0];
    esp_err_t e = fmt ? esp_cam_sensor_set_format(s_cam, fmt) : ESP_ERR_NOT_FOUND;
    if (e != ESP_OK) {
        fail("Camera format", e);
        return false;
    }
    s_w = fmt->width;
    s_h = fmt->height;
    ESP_LOGI(TAG, "sensor ready: %s", fmt->name);

    s_fb_len = (size_t)s_w * s_h * 2;
    for (int i = 0; i < 2; i++) {
        if (!s_fb[i]) s_fb[i] = heap_caps_aligned_calloc(128, 1, s_fb_len, MALLOC_CAP_SPIRAM);
        if (!s_fb[i]) {
            snprintf(s_err, sizeof(s_err), "Out of memory for camera frames");
            return false;
        }
    }
    if (!s_frame_sem) s_frame_sem = xSemaphoreCreateBinary();

    esp_cam_ctlr_csi_config_t csi = {
        .ctlr_id = 0,
        .h_res = s_w,
        .v_res = s_h,
        .lane_bit_rate_mbps = (int)(fmt->mipi_info.mipi_clk / 1000000),
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = (uint8_t)fmt->mipi_info.lane_num,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    if (!s_csi) {
        if ((e = esp_cam_new_csi_ctlr(&csi, &s_csi)) != ESP_OK) {
            s_csi = NULL;
            fail("Camera receiver", e);
            return false;
        }
        esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = on_new_trans, .on_trans_finished = on_trans_done };
        if ((e = esp_cam_ctlr_register_event_callbacks(s_csi, &cbs, NULL)) != ESP_OK ||
            (e = esp_cam_ctlr_enable(s_csi)) != ESP_OK) {
            esp_cam_ctlr_del(s_csi);
            s_csi = NULL;
            fail("Camera receiver", e);
            return false;
        }
    }
    esp_isp_processor_cfg_t isp = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = s_w,
        .v_res = s_h,
        .bayer_order = fmt->isp_info ? bayer_order(fmt->isp_info->isp_v1_info.bayer_type)
                                     : COLOR_RAW_ELEMENT_ORDER_BGGR,
    };
    if ((e = esp_isp_new_processor(&isp, &s_isp)) != ESP_OK) {
        s_isp = NULL;
        fail("Camera ISP", e);
        return false;
    }
    if ((e = esp_isp_enable(s_isp)) != ESP_OK) {
        esp_isp_del_processor(s_isp);
        s_isp = NULL;
        fail("Camera ISP", e);
        return false;
    }
    ae_init();
    return true;
}

/* Take the receiver + ISP down completely. The CSI driver's stop aborts the
 * DMA mid-frame and a restarted controller gave no pictures, so (like
 * esp_video) every start rebuilds them; the sensor stays detected and the
 * frame buffers stay allocated. Order: sensor, receiver, ISP. */
static void teardown(void)
{
    bool was = s_streaming;
    s_streaming = false;
    if (s_cam && was) {
        int off = 0;
        esp_cam_sensor_ioctl(s_cam, ESP_CAM_SENSOR_IOC_S_STREAM, &off);
    }
    if (s_csi) {
        if (was) esp_cam_ctlr_stop(s_csi);
        esp_cam_ctlr_disable(s_csi);
        esp_cam_ctlr_del(s_csi);
        s_csi = NULL;
    }
    if (s_isp) {
        esp_isp_disable(s_isp);
        esp_isp_del_processor(s_isp);
        s_isp = NULL;
    }
    s_ready_hw = false;
    portENTER_CRITICAL(&s_mux);
    s_done = s_held = s_filling = -1;
    portEXIT_CRITICAL(&s_mux);
}

bool bsp_tab5_camera_start(void)
{
    s_err[0] = '\0';
    if (s_streaming) return true;
    if (!s_ready_hw) {
        s_idf_err[0] = '\0';
        s_prev_log = esp_log_set_vprintf(log_hook);
        bool ok = hw_init();
        esp_log_set_vprintf(s_prev_log);
        if (!ok) {
            teardown();
            return false;
        }
        s_ready_hw = true;
    }
    portENTER_CRITICAL(&s_mux);
    s_done = s_held = s_filling = -1;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreTake(s_frame_sem, 0);
    /* receiver first, then the sensor, so the first frame arrives whole */
    esp_err_t e = esp_cam_ctlr_start(s_csi);
    if (e == ESP_OK) {
        s_streaming = true;                     /* teardown() stops the receiver */
        int on = 1;
        e = esp_cam_sensor_ioctl(s_cam, ESP_CAM_SENSOR_IOC_S_STREAM, &on);
    }
    if (e != ESP_OK) {
        fail("Camera start", e);
        teardown();
        return false;
    }
    ESP_LOGI(TAG, "streaming %dx%d (exposure %ld, gain %ld)", s_w, s_h, (long)s_exp, (long)s_gain);
    return true;
}

void bsp_tab5_camera_stop(void)
{
    bool was = s_streaming;
    teardown();
    if (was) ESP_LOGI(TAG, "stopped");
}

bool bsp_tab5_camera_streaming(void) { return s_streaming; }

void bsp_tab5_camera_size(int *w, int *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
}

bool bsp_tab5_camera_grab_gray(uint8_t *out, int w, int h, int step, int timeout_ms)
{
    if (!s_streaming || !out || w <= 0 || h <= 0 || step <= 0) return false;
    if (w * step > s_w) w = s_w / step;
    if (h * step > s_h) h = s_h / step;
    xSemaphoreTake(s_frame_sem, 0);                         /* want a fresh one */
    if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        snprintf(s_err, sizeof(s_err), "No picture from the camera");
        ESP_LOGW(TAG, "no frame in %d ms", timeout_ms);
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    int idx = s_done;
    s_held = idx;
    s_done = -1;
    portEXIT_CRITICAL(&s_mux);
    if (idx < 0) return false;

    const uint16_t *px = (const uint16_t *)s_fb[idx];
    int x0 = (s_w - w * step) / 2, y0 = (s_h - h * step) / 2;
    uint32_t sum = 0;
    for (int y = 0; y < h; y++) {
        uint8_t *o = out + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            /* step > 1: average the step x step block (less noise and moire
             * than picking one pixel) */
            uint32_t acc = 0;
            for (int dy = 0; dy < step; dy++) {
                const uint16_t *row = px + (size_t)(y0 + y * step + dy) * s_w + x0 + x * step;
                for (int dx = 0; dx < step; dx++) {
                    uint16_t c = row[dx];
                    /* RGB565 -> luma: 0.299 R8 + 0.587 G8 + 0.114 B8 with R8 = r*255/31,
                     * G8 = g*255/63, B8 = b*255/31; *256 -> 630 r + 608 g + 240 b */
                    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                    acc += (r * 630 + g * 608 + b * 240) >> 8;      /* 0..255 */
                }
            }
            uint32_t l = acc / (uint32_t)(step * step);
            o[x] = (uint8_t)(l > 255 ? 255 : l);
            sum += o[x];
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_held = -1;
    portEXIT_CRITICAL(&s_mux);
    ae_step(sum / ((uint32_t)w * h));
    return true;
}

#else  /* ------------------------------------------------------------------ simulator */
#include <unistd.h>

#define SIM_CAMERA_FILE TAB5_SD_MOUNT_POINT "/.devos/camera.pgm"

static uint8_t *s_img;
static int s_w, s_h;
static bool s_streaming;

static bool load_pgm(void)
{
    FILE *f = fopen(SIM_CAMERA_FILE, "rb");
    if (!f) return false;
    char magic[3] = "";
    int w = 0, h = 0, maxv = 0;
    bool ok = fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) == 4 && !strcmp(magic, "P5") &&
              w > 0 && h > 0 && w <= 4096 && h <= 4096 && maxv == 255;
    if (ok) {
        fgetc(f);                                           /* the one whitespace */
        uint8_t *img = malloc((size_t)w * h);
        ok = img && fread(img, 1, (size_t)w * h, f) == (size_t)w * h;
        if (ok) {
            free(s_img);
            s_img = img;
            s_w = w;
            s_h = h;
        } else {
            free(img);
        }
    }
    fclose(f);
    return ok;
}

bool bsp_tab5_camera_start(void)
{
    s_err[0] = '\0';
    if (!load_pgm()) {
        snprintf(s_err, sizeof(s_err), "No camera in the simulator (add .devos/camera.pgm)");
        return false;
    }
    s_streaming = true;
    return true;
}

void bsp_tab5_camera_stop(void) { s_streaming = false; }
bool bsp_tab5_camera_streaming(void) { return s_streaming; }

void bsp_tab5_camera_size(int *w, int *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
}

bool bsp_tab5_camera_grab_gray(uint8_t *out, int w, int h, int step, int timeout_ms)
{
    if (!s_streaming || !s_img || !out || w <= 0 || h <= 0 || step <= 0) return false;
    usleep(66000);                                          /* ~15 fps */
    (void)timeout_ms;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t acc = 0;
            for (int dy = 0; dy < step; dy++) {
                for (int dx = 0; dx < step; dx++) {
                    int sx = (s_w - w * step) / 2 + x * step + dx, sy = (s_h - h * step) / 2 + y * step + dy;
                    acc += (sx >= 0 && sy >= 0 && sx < s_w && sy < s_h) ? s_img[(size_t)sy * s_w + sx] : 255;
                }
            }
            out[(size_t)y * w + x] = (uint8_t)(acc / (uint32_t)(step * step));
        }
    }
    return true;
}
#endif

const char *bsp_tab5_camera_error(void) { return s_err; }
