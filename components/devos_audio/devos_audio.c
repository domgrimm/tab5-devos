/* devos_audio: see devos_audio.h.
 *
 * Hardware set-up follows M5Stack's Tab5 BSP: I2S1 master, TX standard mode,
 * RX 4-slot TDM at 48 kHz / 16 bit; ES7210 frames arrive as
 * [MIC-L, AEC reference, MIC-R, headset mic]. */
#include "devos_audio.h"
#include "devos_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HW_RATE   48000
#define DECIM     (HW_RATE / DEVOS_AUDIO_RATE)      /* 3 */
#define CHUNK     480                               /* 10 ms of hardware frames */
#define FLUSH     8000                              /* samples per SD write (0.5 s) */

static char s_err[96];
static volatile bool s_rec, s_play, s_stop_req, s_play_stop_req;
static volatile uint32_t s_rec_samples, s_play_pos, s_play_len;
static volatile float s_level;
static int s_volume = 70;
static float s_gain_db = 30.0f;
static char s_path[160];

const char *devos_audio_error(void) { return s_err; }
bool devos_audio_recording(void) { return s_rec; }
bool devos_audio_playing(void) { return s_play; }
uint32_t devos_audio_record_ms(void) { return (uint32_t)((uint64_t)s_rec_samples * 1000 / DEVOS_AUDIO_RATE); }
float devos_audio_level(void) { return s_rec ? s_level : 0.0f; }
int devos_audio_volume(void) { return s_volume; }

void devos_audio_play_pos(uint32_t *pos, uint32_t *len)
{
    if (pos) *pos = (uint32_t)((uint64_t)s_play_pos * 1000 / DEVOS_AUDIO_RATE);
    if (len) *len = (uint32_t)((uint64_t)s_play_len * 1000 / DEVOS_AUDIO_RATE);
}

/* ------------------------------------------------------------------ WAV */
static void wav_header(uint8_t *h, uint32_t samples, uint32_t rate, int channels)
{
    uint32_t data = samples * 2 * (uint32_t)channels;
    uint32_t byte_rate = rate * 2 * (uint32_t)channels;
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s = 1;
    memcpy(h + 20, &s, 2);                          /* PCM */
    s = (uint16_t)channels;
    memcpy(h + 22, &s, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    s = (uint16_t)(2 * channels);
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

/* Opens a 16-bit PCM WAV; returns the file at the data with its format. */
static FILE *wav_open(const char *path, uint32_t *rate, int *channels, uint32_t *samples)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return NULL;
    }
    bool fmt_ok = false;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t len;
        memcpy(&len, ch + 4, 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) break;
            uint16_t fmt, chans, bits;
            memcpy(&fmt, fm, 2);
            memcpy(&chans, fm + 2, 2);
            memcpy(rate, fm + 4, 4);
            memcpy(&bits, fm + 14, 2);
            fmt_ok = fmt == 1 && bits == 16 && (chans == 1 || chans == 2) && *rate && HW_RATE % *rate == 0;
            *channels = chans;
            if (len > 16) fseek(f, (long)(len - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (!fmt_ok) break;
            /* a recording cut short (power loss) never got its sizes written:
             * take whatever follows */
            long here = ftell(f);
            fseek(f, 0, SEEK_END);
            long end = ftell(f);
            fseek(f, here, SEEK_SET);
            if (end > here && (len == 0 || (long)len > end - here)) len = (uint32_t)(end - here);
            *samples = len / (2 * (uint32_t)*channels);
            return f;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    return NULL;
}

uint32_t devos_audio_wav_ms(const char *path)
{
    uint32_t rate = 0, samples = 0;
    int ch = 1;
    FILE *f = wav_open(path, &rate, &ch, &samples);
    if (!f) return 0;
    fclose(f);
    return (uint32_t)((uint64_t)samples * 1000 / rate);
}

#ifdef ESP_PLATFORM
/* ================================================================== device */
#include "bsp_tab5.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";
#define PIN_MCLK 30
#define PIN_BCLK 27
#define PIN_WS   29
#define PIN_DOUT 26
#define PIN_DIN  28

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_hw_ok, s_hw_failed;
static SemaphoreHandle_t s_wake;
static TaskHandle_t s_task;

static bool hw_init(void)
{
    if (s_hw_ok) return true;
    if (s_hw_failed) return false;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bsp_tab5_i2c_bus_internal();
    if (!bus) {
        snprintf(s_err, sizeof(s_err), "I2C bus not ready");
        return false;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;                               /* silence when nothing plays */
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "No I2S channel for audio");
        s_hw_failed = true;
        return false;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(HW_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    i2s_tdm_config_t tdm = {
        .clk_cfg = { .sample_rate_hz = HW_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                     .bclk_div = 8 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_STEREO,
                      .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                      .ws_width = I2S_TDM_AUTO_WS_WIDTH, .bit_shift = true, .total_slot = I2S_TDM_AUTO_SLOT_NUM },
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    esp_err_t e = i2s_channel_init_std_mode(s_tx, &std);
    if (e == ESP_OK) e = i2s_channel_enable(s_tx);
    if (e == ESP_OK) e = i2s_channel_init_tdm_mode(s_rx, &tdm);
    if (e == ESP_OK) e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "I2S set-up failed (%s)", esp_err_to_name(e));
        s_hw_failed = true;
        return false;
    }
    audio_codec_i2s_cfg_t icfg = { .port = I2S_NUM_1, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&icfg);

    audio_codec_i2c_cfg_t c1 = { .port = 1, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl_spk = audio_codec_new_i2c_ctrl(&c1);
    es8388_codec_cfg_t es8388 = {
        .ctrl_if = ctrl_spk,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .pa_pin = -1,                                   /* amp enable is IO expander 0x43 P1, on since boot */
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *spk_if = ctrl_spk ? es8388_codec_new(&es8388) : NULL;

    audio_codec_i2c_cfg_t c2 = { .port = 1, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl_mic = audio_codec_new_i2c_ctrl(&c2);
    es7210_codec_cfg_t es7210 = { .ctrl_if = ctrl_mic };
    es7210.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t *mic_if = ctrl_mic ? es7210_codec_new(&es7210) : NULL;
    if (!data_if || !spk_if || !mic_if) {
        snprintf(s_err, sizeof(s_err), "Audio codecs didn't answer (ES8388 / ES7210)");
        s_hw_failed = true;
        return false;
    }
    esp_codec_dev_cfg_t dspk = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = spk_if, .data_if = data_if };
    esp_codec_dev_cfg_t dmic = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_if, .data_if = data_if };
    s_spk = esp_codec_dev_new(&dspk);
    s_mic = esp_codec_dev_new(&dmic);
    esp_codec_dev_sample_info_t fs_mic = { .bits_per_sample = 16, .channel = 4, .sample_rate = HW_RATE };
    esp_codec_dev_sample_info_t fs_spk = { .bits_per_sample = 16, .channel = 2, .sample_rate = HW_RATE };
    if (!s_spk || !s_mic || esp_codec_dev_open(s_mic, &fs_mic) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(s_spk, &fs_spk) != ESP_CODEC_DEV_OK) {
        snprintf(s_err, sizeof(s_err), "Couldn't open the audio codecs");
        s_hw_failed = true;
        return false;
    }
    esp_codec_dev_set_in_gain(s_mic, s_gain_db);
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    s_hw_ok = true;
    ESP_LOGI(TAG, "ES7210 + ES8388 ready at %d Hz", HW_RATE);
    return true;
}

void devos_audio_set_volume(int pct)
{
    s_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_volume);
}

void devos_audio_set_gain(float db)
{
    s_gain_db = db < 0 ? 0 : db > 36 ? 36 : db;
    if (s_mic) esp_codec_dev_set_in_gain(s_mic, s_gain_db);
}

static void do_record(void)
{
    FILE *f = fopen(s_path, "wb");
    int16_t *in = heap_caps_malloc(CHUNK * 4 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *pcm = heap_caps_malloc(FLUSH * sizeof(int16_t) + 64, MALLOC_CAP_SPIRAM);
    if (!f || !in || !pcm) {
        snprintf(s_err, sizeof(s_err), f ? "Out of memory" : "Couldn't create the recording (SD card?)");
        if (f) fclose(f);
        free(in);
        free(pcm);
        s_rec = false;
        return;
    }
    uint8_t hdr[44];
    wav_header(hdr, 0, DEVOS_AUDIO_RATE, 1);
    fwrite(hdr, 1, 44, f);
    uint32_t total = 0, buffered = 0;
    float hp_x = 0, hp_y = 0;                          /* DC-blocking high-pass */
    int32_t acc = 0;
    int phase = 0;
    bool ok = true;
    while (!s_stop_req && total < (uint32_t)DEVOS_AUDIO_MAX_S * DEVOS_AUDIO_RATE) {
        if (esp_codec_dev_read(s_mic, in, CHUNK * 4 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            snprintf(s_err, sizeof(s_err), "The microphones stopped");
            ok = false;
            break;
        }
        double sq = 0;
        int n = 0;
        for (int i = 0; i < CHUNK; i++) {
            float x = ((float)in[i * 4 + 0] + (float)in[i * 4 + 2]) * 0.5f;     /* MIC-L + MIC-R */
            float y = x - hp_x + 0.995f * hp_y;
            hp_x = x;
            hp_y = y;
            acc += (int32_t)y;
            if (++phase == DECIM) {                     /* 48 -> 16 kHz: average of 3 */
                int32_t v = acc / DECIM;
                acc = 0;
                phase = 0;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[buffered++] = (int16_t)v;
                sq += (double)v * v;
                n++;
            }
        }
        if (n) {
            float rms = (float)sqrt(sq / n) / 32768.0f;
            float lvl = rms * 4.0f;                     /* speech sits around 0.05 rms */
            s_level = lvl > 1 ? 1 : lvl;
        }
        if (buffered >= FLUSH) {
            if (fwrite(pcm, 2, buffered, f) != buffered) {
                snprintf(s_err, sizeof(s_err), "SD card write failed");
                ok = false;
                break;
            }
            total += buffered;
            buffered = 0;
        }
        s_rec_samples = total + buffered;
    }
    if (buffered && ok) {
        fwrite(pcm, 2, buffered, f);
        total += buffered;
    }
    wav_header(hdr, total, DEVOS_AUDIO_RATE, 1);
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, 44, f);
    fclose(f);
    free(in);
    free(pcm);
    s_rec_samples = total;
    s_rec = false;
    s_level = 0;
    ESP_LOGI(TAG, "recorded %u ms to %s", (unsigned)devos_audio_record_ms(), s_path);
}

static void do_play(void)
{
    uint32_t rate = 0, samples = 0;
    int ch = 1;
    FILE *f = wav_open(s_path, &rate, &ch, &samples);
    int16_t *src = malloc(1024 * 2 * sizeof(int16_t));
    int16_t *out = heap_caps_malloc(1024 * 6 * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!f || !src || !out) {
        snprintf(s_err, sizeof(s_err), f ? "Out of memory" : "Not a 16-bit WAV this can play");
        if (f) fclose(f);
        free(src);
        free(out);
        s_play = false;
        return;
    }
    int up = HW_RATE / (int)rate;
    s_play_len = (uint32_t)((uint64_t)samples * DEVOS_AUDIO_RATE / rate);
    uint32_t done = 0;
    while (!s_play_stop_req && done < samples) {
        size_t want = samples - done < 1024 ? samples - done : 1024;
        size_t got = fread(src, 2 * (size_t)ch, want, f);
        if (!got) break;
        size_t o = 0;
        for (size_t i = 0; i < got; i++) {
            int16_t l = src[i * ch], r = ch == 2 ? src[i * 2 + 1] : l;
            for (int k = 0; k < up; k++) {              /* repeat to 48 kHz stereo */
                out[o++] = l;
                out[o++] = r;
            }
        }
        if (esp_codec_dev_write(s_spk, out, (int)(o * sizeof(int16_t))) != ESP_CODEC_DEV_OK) break;
        done += (uint32_t)got;
        s_play_pos = (uint32_t)((uint64_t)done * DEVOS_AUDIO_RATE / rate);
    }
    fclose(f);
    free(src);
    free(out);
    s_play = false;
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        if (s_rec) do_record();
        else if (s_play) do_play();
    }
}

static bool task_ready(void)
{
    if (!hw_init()) return false;
    if (!s_task) {
        s_wake = xSemaphoreCreateBinary();
        /* Core 1: next to the SD card I/O, away from the network stack */
        if (xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 4, &s_task, DEVOS_CORE_UI_INPUT) != pdPASS) {
            snprintf(s_err, sizeof(s_err), "Couldn't start the audio task");
            return false;
        }
    }
    return true;
}

int devos_audio_record_start(const char *path)
{
    s_err[0] = '\0';
    if (s_rec || s_play) return -1;
    if (!task_ready()) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_rec_samples = 0;
    s_stop_req = false;
    s_rec = true;
    xSemaphoreGive(s_wake);
    return 0;
}

int devos_audio_play(const char *path)
{
    s_err[0] = '\0';
    if (s_rec || s_play) return -1;
    if (!task_ready()) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_play_pos = s_play_len = 0;
    s_play_stop_req = false;
    s_play = true;
    xSemaphoreGive(s_wake);
    return 0;
}

void devos_audio_record_stop(void) { s_stop_req = true; }
void devos_audio_play_stop(void) { s_play_stop_req = true; }

#else
/* ================================================================== simulator */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void devos_audio_set_volume(int pct) { s_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
void devos_audio_set_gain(float db) { s_gain_db = db; }

static void *sim_rec(void *arg)
{
    (void)arg;
    FILE *f = fopen(s_path, "wb");
    if (!f) {
        snprintf(s_err, sizeof(s_err), "Couldn't create the recording");
        s_rec = false;
        return NULL;
    }
    uint8_t hdr[44];
    wav_header(hdr, 0, DEVOS_AUDIO_RATE, 1);
    fwrite(hdr, 1, 44, f);
    int16_t buf[1600];
    uint32_t total = 0;
    double ph = 0;
    while (!s_stop_req) {
        for (int i = 0; i < 1600; i++) {               /* 100 ms of a quiet 440 Hz tone */
            ph += 2 * M_PI * 440.0 / DEVOS_AUDIO_RATE;
            buf[i] = (int16_t)(1500 * sin(ph));
        }
        fwrite(buf, 2, 1600, f);
        total += 1600;
        s_rec_samples = total;
        s_level = 0.2f + 0.6f * (float)((total / 1600) % 7) / 7.0f;
        usleep(100000);
    }
    wav_header(hdr, total, DEVOS_AUDIO_RATE, 1);
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, 44, f);
    fclose(f);
    s_rec = false;
    s_level = 0;
    return NULL;
}

static void *sim_play(void *arg)
{
    (void)arg;
    uint32_t rate = 0, samples = 0;
    int ch = 1;
    FILE *f = wav_open(s_path, &rate, &ch, &samples);
    if (!f) {
        snprintf(s_err, sizeof(s_err), "Not a 16-bit WAV this can play");
        s_play = false;
        return NULL;
    }
    fclose(f);
    s_play_len = (uint32_t)((uint64_t)samples * DEVOS_AUDIO_RATE / rate);
    for (uint32_t t = 0; t < s_play_len && !s_play_stop_req; t += 1600) {
        s_play_pos = t;
        usleep(100000);
    }
    s_play = false;
    return NULL;
}

int devos_audio_record_start(const char *path)
{
    s_err[0] = '\0';
    if (s_rec || s_play) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_rec_samples = 0;
    s_stop_req = false;
    s_rec = true;
    pthread_t t;
    pthread_create(&t, NULL, sim_rec, NULL);
    pthread_detach(t);
    return 0;
}

int devos_audio_play(const char *path)
{
    s_err[0] = '\0';
    if (s_rec || s_play) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_play_pos = s_play_len = 0;
    s_play_stop_req = false;
    s_play = true;
    pthread_t t;
    pthread_create(&t, NULL, sim_play, NULL);
    pthread_detach(t);
    return 0;
}

void devos_audio_record_stop(void) { s_stop_req = true; }
void devos_audio_play_stop(void) { s_play_stop_req = true; }
#endif
