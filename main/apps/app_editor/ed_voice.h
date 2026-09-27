#pragma once
/* Editor: voice memos (the scratchpad's audio side). Recording, playback, the
 * memo list (Ctrl+M) with its settings, and optional transcription through a
 * Whisper-compatible HTTP endpoint. The editor owns the text: the module
 * hands back lines to insert through the callback given to ed_voice_init. */
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Insert `line` on its own line after the line containing `after` (a memo
 * file name), or at the cursor if `after` is NULL or not found. */
typedef void (*ed_voice_insert_fn)(const char *after, const char *line);

void ed_voice_init(lv_obj_t *screen, ed_voice_insert_fn insert);

/* Recording into <memo_dir> (absolute). Returns false with a message. */
bool ed_voice_record_start(const char *memo_dir, char *msg, size_t cap);
/* Stop; the Markdown link line (relative "memos/...") is inserted once the
 * file is closed, then transcribed if that's switched on. */
void ed_voice_record_stop(void);
bool ed_voice_recording(void);

/* Play / stop an absolute path. */
bool ed_voice_play(const char *abs_path, char *msg, size_t cap);
void ed_voice_stop(void);
bool ed_voice_playing(void);

/* The memo list for a folder of memos (absolute). */
void ed_voice_open_list(const char *memo_dir);
bool ed_voice_list_open(void);
/* Keys while a voice dialog is open (takes them all). */
bool ed_voice_key(uint32_t key, uint8_t mods);

/* Every editor tick (250 ms): finishes recordings, collects transcripts. */
void ed_voice_tick(void);
/* "REC 0:12  |||||" / "PLAY 0:05 / 0:23" / "Transcribing..." or "". */
void ed_voice_status(char *out, size_t cap);
/* Message from the last background step (transcript done / failed), "" if none. */
const char *ed_voice_take_message(void);
/* Hint line for the keys footer while a dialog is open. */
const char *ed_voice_keys(void);
