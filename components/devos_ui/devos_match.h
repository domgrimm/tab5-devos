#pragma once

/* devos_match: the command palette's search, plain C (no LVGL) so the host
 * test (tools/palette_test.c) covers it.
 *
 * Every word of the query has to match the title or the keywords, ignoring
 * case and punctuation ("adsb" finds "ADS-B Radar"). Each word scores, best
 * first: the start of the title, the start of a title word, the start of a
 * keyword, anywhere in the title, anywhere in the keywords, and last its
 * letters in order in the title ("dkr" finds "Docker"). Little words (the,
 * to, for ...) don't count as word starts.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -1 = no match; otherwise higher is better (an empty query scores 0). */
int devos_match_score(const char *query, const char *title, const char *keywords);

#ifdef __cplusplus
}
#endif
