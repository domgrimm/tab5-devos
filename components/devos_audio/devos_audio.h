#pragma once

/* devos_audio: voice memos on the Tab5's audio hardware (no LVGL).
 *
 * ES7210 4-channel mic ADC + ES8388 codec (speaker amp on the IO expander)
 * on one full-duplex I2S bus at 48 kHz, set up on first use. Recording mixes
 * the two front mics to mono and writes 16 kHz 16-bit WAV to the SD card
 * while it records; playback reads such a WAV back to the speaker. One
 * audio task on Core 1 (with the SD card I/O) does both.
 *
 * The simulator has no audio: recordings are a quiet test tone, playback
 * just runs the clock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_AUDIO_RATE     16000      /* WAV sample rate */
#define DEVOS_AUDIO_MAX_S    (30 * 60)  /* longest recording */

/* Start / stop recording to a new WAV file. */
int devos_audio_record_start(const char *path);
void devos_audio_record_stop(void);
bool devos_audio_recording(void);
uint32_t devos_audio_record_ms(void);

int devos_audio_play(const char *path);
void devos_audio_play_stop(void);
bool devos_audio_playing(void);
void devos_audio_play_pos(uint32_t *pos_ms, uint32_t *len_ms);

/* Input level of the last 50 ms, 0..1 (recording only). */
float devos_audio_level(void);
/* 0..100 */
void devos_audio_set_volume(int pct);
int devos_audio_volume(void);
/* Mic gain in dB (0..36). */
void devos_audio_set_gain(float db);

/* Why the last start failed / what went wrong ("" when fine). */
const char *devos_audio_error(void);
/* Length of a 16-bit mono WAV file in ms (0 if not one). */
uint32_t devos_audio_wav_ms(const char *path);

#ifdef __cplusplus
}
#endif
