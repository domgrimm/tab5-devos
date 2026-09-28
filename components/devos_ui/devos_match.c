/* devos_match: see devos_match.h. */
#include "devos_match.h"

#include <stdbool.h>
#include <string.h>

#define WORD_MAX 32

static bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c & 0x80);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* text without case or punctuation: "ADS-B Radar" -> "adsbradar" */
static void squash(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; s && *s && n + 1 < cap; s++) {
        if (word_char(*s)) out[n++] = lower(*s);
    }
    out[n] = '\0';
}

/* Little words don't count as word starts: "the" should find the theme,
 * not "Open the scratchpad". */
static bool little_word(const char *w, size_t n)
{
    static const char *const little[] = { "a", "an", "and", "for", "in", "of", "or", "the", "to" };
    for (size_t i = 0; i < sizeof(little) / sizeof(little[0]); i++) {
        if (strlen(little[i]) != n) continue;
        size_t k = 0;
        while (k < n && lower(w[k]) == little[i][k]) k++;
        if (k == n) return true;
    }
    return false;
}

/* Does a word of `s` start with `tok` (squashed)? */
static bool word_prefix(const char *s, const char *tok)
{
    size_t tl = strlen(tok);
    while (s && *s) {
        while (*s && !word_char(*s)) s++;
        if (!*s) break;
        const char *w = s;
        size_t len = 0;
        while (w[len] && word_char(w[len])) len++;
        size_t i = 0;
        while (i < len && i < tl && lower(w[i]) == tok[i]) i++;
        if (i == tl && !little_word(w, len)) return true;
        s = w + len;
    }
    return false;
}

static bool subsequence(const char *s, const char *tok)
{
    for (; *s && *tok; s++) {
        if (*s == *tok) tok++;
    }
    return !*tok;
}

static int token_score(const char *tok, const char *title, const char *title_sq, const char *keywords,
                       const char *kw_sq)
{
    if (!strncmp(title_sq, tok, strlen(tok))) return 100;
    if (word_prefix(title, tok)) return 80;
    if (word_prefix(keywords, tok)) return 60;
    if (strstr(title_sq, tok)) return 40;
    if (strstr(kw_sq, tok)) return 30;
    if (strlen(tok) >= 2 && subsequence(title_sq, tok)) return 10;
    return -1;
}

int devos_match_score(const char *query, const char *title, const char *keywords)
{
    char title_sq[128], kw_sq[256];
    squash(title, title_sq, sizeof(title_sq));
    squash(keywords, kw_sq, sizeof(kw_sq));
    int total = 0;
    const char *q = query ? query : "";
    while (*q) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;
        char tok[WORD_MAX];
        size_t n = 0;
        for (; *q && *q != ' ' && *q != '\t'; q++) {
            if (word_char(*q) && n + 1 < sizeof(tok)) tok[n++] = lower(*q);
        }
        tok[n] = '\0';
        if (!n) continue;                   /* punctuation only */
        int s = token_score(tok, title ? title : "", title_sq, keywords ? keywords : "", kw_sq);
        if (s < 0) return -1;
        total += s;
    }
    return total;
}
