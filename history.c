// history.c
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include "debug.h"
//#include <readline/history.h>

#pragma region small_helpers
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static void *xreallocarray(void *ptr, size_t nmemb, size_t size) {
    if (nmemb && size > SIZE_MAX / nmemb) return NULL;
    return realloc(ptr, nmemb * size);
}
static int is_blank(const char *s) {
    for (; *s; ++s) if (!isspace((unsigned char)*s)) return 0;
    return 1;
}
static void rtrim_spaces(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}
#pragma endregion

size_t history_count(const History *h) { return h ? h->len : 0; }
const HistEntry* get_history(const History *h, size_t idx) {
    if (!h || idx >= h->len) return NULL;
    return &h->v[idx];
}

static int ensure_cap(History *h, size_t need) {
    LOG(LOG_LEVEL_INFO, "checking need");
    if (need <= h->cap) return 0;
    size_t new_cap = h->cap ? h->cap * 2 : 128;
    while (new_cap < need) new_cap *= 2;
    HistEntry *nv = (HistEntry*)xreallocarray(h->v, new_cap, sizeof(HistEntry));
    if (!nv) return -1;
    h->v = nv; h->cap = new_cap;
    return 0;
}

static void free_entry(HistEntry *e) {
    if (!e) return;
    free(e->line);
    memset(e, 0, sizeof(*e));
}

void history_dispose(History *h) {
    if (!h) return;
    for (size_t i = 0; i < h->len; ++i) free_entry(&h->v[i]);
    free(h->v); h->v = NULL; h->len = h->cap = 0;
    free(h->path); h->path = NULL;
    h->next_id = 1;
}

int history_init(History *h, const char *path, size_t max_entries, int flags) {
    if (!h) { errno = EINVAL; return -1; }
    memset(h, 0, sizeof(*h));
    h->max = max_entries ? max_entries : 1000;
    h->flags = flags;
    h->next_id = 1;
    if (path) {
        h->path = xstrdup(path);
        if (!h->path) return -1;
    }
    return 0;
}

// Format: one entry per line -> "<epoch>\t<status>\t<command>\n"
// Commands are single-line from readline, so tabs/newlines are rare; we escape backslash, tab, newline.
static void fescape(FILE *f, const char *s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\') { fputs("\\\\", f); }
        else if (c == '\t') { fputs("\\t", f); }
        else if (c == '\n') { fputs("\\n", f); }
        else { fputc(c, f); }
    }
}

static char *unescape(const char *s) {
    size_t n = strlen(s);
    char *out = (char*)malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[++i];
            if (c == 'n') out[j++] = '\n';
            else if (c == 't') out[j++] = '\t';
            else out[j++] = c;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

int history_save(const History *h) {
    if (!h || !h->path) { errno = EINVAL; return -1; }
    // Write atomically: path.tmp -> rename
    char tmppath[4096];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", h->path);
    FILE *f = fopen(tmppath, "w");
    if (!f) return -1;

    // Trim to max before writing (mirror in-memory policy)
    size_t start = (h->len > h->max) ? (h->len - h->max) : 0;
    for (size_t i = start; i < h->len; ++i) {
        const HistEntry *e = &h->v[i];
        // epoch\tstatus\tcommand\n
        fprintf(f, "%ld\t%d\t", (long)e->when, e->status);
        fescape(f, e->line ? e->line : "");
        fputc('\n', f);
    }
    if (fclose(f) != 0) { remove(tmppath); return -1; }
    if (rename(tmppath, h->path) != 0) { remove(tmppath); return -1; }
    return 0;
}

int history_load(History *h) {
    if (!h || !h->path) { errno = EINVAL; return -1; }
    FILE *f = fopen(h->path, "r");
    if (!f) return (errno == ENOENT) ? 0 : -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n && (line[n-1] == '\n')) line[n-1] = '\0';
        // parse "<epoch>\t<status>\t<cmd>"
        char *p = line;
        char *tab1 = strchr(p, '\t'); if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t'); if (!tab2) continue;
        *tab2 = '\0';
        long epoch = strtol(p, NULL, 10);
        int status = (int)strtol(tab1 + 1, NULL, 10);
        char *cmd_unesc = unescape(tab2 + 1);
        if (!cmd_unesc) { free(line); fclose(f); return -1; }

        if (ensure_cap(h, h->len + 1) != 0) { free(cmd_unesc); free(line); fclose(f); return -1; }
        HistEntry *e = &h->v[h->len++];
        e->id = h->next_id++;
        e->when = (time_t)epoch;
        e->status = status;
        e->line = cmd_unesc;
    }
    free(line);
    fclose(f);

    // Enforce max cap post-load
    if (h->len > h->max) {
        size_t drop = h->len - h->max;
        for (size_t i = 0; i < drop; ++i) free_entry(&h->v[i]);
        memmove(h->v, h->v + drop, (h->len - drop) * sizeof(HistEntry));
        h->len -= drop;
    }
    return 0;
}

int history_stifle(History *h, size_t max_entries) {
    if (!h) { errno = EINVAL; return -1; }
    h->max = max_entries ? max_entries : h->max;
    if (h->len <= h->max) return 0;
    size_t drop = h->len - h->max;
    for (size_t i = 0; i < drop; ++i) free_entry(&h->v[i]);
    memmove(h->v, h->v + drop, (h->len - drop) * sizeof(HistEntry));
    h->len -= drop;
    return 0;
}

static int should_ignore(const History *h, const char *line) {
    if (!line) return 1;
    if ((h->flags & HISTORY_IGNORE_EMPTY) && is_blank(line)) return 1;

    if ((h->flags & HISTORY_IGNORE_SPACE) && line[0] == ' ') {
        fprintf(stderr, "[ignorespace] len=%zu new=\"%s\"\n",
                h->len, line ? line : "<null>");
        return 1;
    }

    if (h->flags & HISTORY_TRIM_TRAILING) {
        // we'll trim in a copy later; decision not based on trailing spaces alone
    }

    if (h->flags & HISTORY_IGNORE_DUPS) {
        fprintf(stderr, "[dupchk] len=%zu new=\"%s\"\n",
                h->len, line ? line : "<null>");
        if (h->len) {
            const char *prev = h->v[h->len - 1].line;
            fprintf(stderr, "[dupchk] prev=\"%s\" new=\"%s\"\n",
                    prev ? prev : "<null>",
                    line ? line : "<null>");
            if (prev && strcmp(prev, line) == 0) return 1;
        }
    }
    return 0;
}

HistoryAddResult history_add(History *h, const char *line) {
    HistoryAddResult res = {0, 0};
    if (!h || !line) { errno = EINVAL; return res; }

    char *work = xstrdup(line);
    if (!work) return res;
    if (h->flags & HISTORY_TRIM_TRAILING) rtrim_spaces(work);

    if (should_ignore(h, work)) { free(work); return res; }

    if (ensure_cap(h, h->len + 1) != 0) { free(work); return res; }

    HistEntry *e = &h->v[h->len++];
    e->id = h->next_id++;
    e->when = time(NULL);
    e->status = -1;
    e->line = work;

    // Mirror into readline memory history
    // Caller must include <readline/history.h> in compilation unit using this function.
    extern void add_history(const char *); // avoid including in header
    add_history(e->line);
    res.id = e->id;
    res.added_to_readline = 1;

    // Enforce cap immediately
    history_stifle(h, h->max);
    return res;
}

int history_set_status_by_id(History *h, uint64_t id, int status) {
    if (!h || id == 0) { errno = EINVAL; return -1; }
    for (size_t i = h->len; i-- > 0; ) {
        if (h->v[i].id == id) { h->v[i].status = status; return 0; }
    }
    errno = ENOENT;
    return -1;
}

int history_set_status_last(History *h, int status) {
    if (!h || h->len == 0) { errno = EINVAL; return -1; }
    h->v[h->len - 1].status = status;
    return 0;
}

// XDG: $XDG_STATE_HOME/thrash/history or $HOME/.thrash_history
int history_default_path(char *out, size_t out_sz) {
    if (!out || out_sz == 0) { errno = EINVAL; return -1; }
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
#ifdef _WIN32
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata && snprintf(out, out_sz, "%s\\thrash\\history", appdata) < (int)out_sz) return 0;
    if (home && snprintf(out, out_sz, "%s\\AppData\\Local\\thrash\\history", home) < (int)out_sz) return 0;
#else
    if (xdg && snprintf(out, out_sz, "%s/thrash/history", xdg) < (int)out_sz) return 0;
    if (home && snprintf(out, out_sz, "%s/.thrash_history", home) < (int)out_sz) return 0;
#endif
    errno = ENOENT;
    return -1;
}