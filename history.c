// Enable POSIX APIs like getline(), strdup(), etc. by setting feature macros
#define _POSIX_C_SOURCE 200809L

// Local project headers
#include "history.h"  // Declares History, HistEntry, and related API
#include "debug.h"    // Declares LOG() macro and log levels

// Standard library headers
#include <stdlib.h>   // malloc(), realloc(), free(), size_t, NULL
#include <string.h>   // strlen(), memcpy(), memset(), strchr()
#include <errno.h>    // errno constants (EINVAL, ENOENT, etc.)
#include <ctype.h>    // isspace()
#include <stdio.h>    // FILE I/O (fopen, fprintf, etc.)

// This would pull in GNU readline history support if used
//#include <readline/history.h>

#pragma region small_helpers
// strdup() clone: duplicates a string with malloc.
// Returns NULL if source is NULL or memory allocation fails.
// Allocates exact bytes needed (length + 1) and copies the NUL terminator.
static char *xstrdup(const char *s) {
    if (!s) return NULL;                // Guard: don't try strlen(NULL)
    size_t n = strlen(s) + 1;           // Bytes needed: length + NUL terminator
    char *p = (char*)malloc(n);         // Allocate exact size
    if (p) memcpy(p, s, n);             // Copy entire string (including NUL)
    return p;                           // May be NULL if malloc failed
}

// Safe realloc array: checks for multiplication overflow before realloc.
// Avoids undefined behaviour if nmemb * size would wrap past SIZE_MAX.
static void *xreallocarray(void *ptr, size_t nmemb, size_t size) {
    if (nmemb && size > SIZE_MAX / nmemb) return NULL; // overflow guard
    return realloc(ptr, nmemb * size);                 // May resize or move block
}

// Returns nonzero if the string is all spaces/tabs/newlines; 0 otherwise.
static int is_blank(const char *s) {
    for (; *s; ++s)
        if (!isspace((unsigned char)*s)) // Any non-space char → not blank
            return 0;
    return 1;  // All characters were whitespace
}

// Right-trim: remove trailing spaces/tabs/newlines from a mutable string.
static void rtrim_spaces(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1]))
        s[--n] = '\0';  // Overwrite trailing whitespace with NUL terminator
}
#pragma endregion

// Return number of history entries; safe for NULL pointer.
size_t history_count(const History *h) {
    return h ? h->len : 0;
}

// Retrieve pointer to entry at given index; NULL if out-of-bounds.
const HistEntry* get_history(const History *h, size_t idx) {
    if (!h || idx >= h->len) return NULL;
    return &h->v[idx];  // Direct pointer into h->v array
}

// Ensure capacity for 'need' entries; grow exponentially if necessary.
// Returns 0 on success, -1 on allocation failure.
static int check_cap(History *h, size_t need) {
    LOG(LOG_LEVEL_INFO, "checking need");
    if (need <= h->cap) return 0;        // Already enough capacity

    // Start with double capacity, or 128 if starting from zero
    size_t new_cap = h->cap ? h->cap * 2 : 128;
    while (new_cap < need)               // Keep doubling until large enough
        new_cap *= 2;

    // Reallocate underlying array to new_cap entries
    HistEntry *nv = (HistEntry*)xreallocarray(h->v, new_cap, sizeof(HistEntry));
    if (!nv) return -1;                   // Allocation failed
    h->v = nv;
    h->cap = new_cap;
    return 0;
}

// Free dynamic fields inside a HistEntry and zero the struct.
// Does not free the HistEntry pointer itself (caller controls array).
static void free_entry(HistEntry *e) {
    if (!e) return;
    free(e->line);                        // Release string
    memset(e, 0, sizeof(*e));             // Zero all fields (id, status, etc.)
}

// Free entire history: all entries, storage array, and path string.
void history_dispose(History *h) {
    if (!h) return;
    for (size_t i = 0; i < h->len; ++i)
        free_entry(&h->v[i]);              // Clean entry payloads
    free(h->v);                            // Release array block
    h->v = NULL;
    h->len = h->cap = 0;
    free(h->path);                         // Release saved path
    h->path = NULL;
    h->next_id = 1;                        // Reset ID counter
}

// Initialize History struct fields; optional save path and max_entries limit.
int history_init(History *h, const char *path, size_t max_entries, int flags) {
    if (!h) { errno = EINVAL; return -1; } // Must pass valid target
    memset(h, 0, sizeof(*h));              // Reset all fields to 0/NULL
    h->max = max_entries ? max_entries : 1000; // Default = 1000 entries
    h->flags = flags;
    h->next_id = 1;
    if (path) {
        h->path = xstrdup(path);           // Duplicate for owned storage
        if (!h->path) return -1;           // Propagate allocation failure
    }
    return 0;
}

// Write escaped version of s to FILE*: escape backslash, tab, newline.
// Ensures save file can be parsed back unambiguously.
static void fescape(FILE *f, const char *s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\')       { fputs("\\\\", f); } // Literal backslash
        else if (c == '\t')  { fputs("\\t",  f); } // Literal tab
        else if (c == '\n')  { fputs("\\n",  f); } // Literal newline
        else                 { fputc(c, f);       } // Pass through
    }
}

// Decode escape sequences from storage form to actual chars.
// Allocates a new buffer; caller must free.
static char *unescape(const char *s) {
    size_t n = strlen(s);
    char *out = (char*)malloc(n + 1);     // Worst case: no escapes → same length
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\\' && i + 1 < n) {  // Found escape marker
            char c = s[++i];              // Examine next char
            if      (c == 'n') out[j++] = '\n';
            else if (c == 't') out[j++] = '\t';
            else               out[j++] = c; // Unknown escape: take literally
        } else {
            out[j++] = s[i];              // Normal character
        }
    }
    out[j] = '\0';
    return out;
}

// Save history entries to disk, writing to <path>.tmp then renaming.
// Enforces max entry limit before writing.
int history_save(const History *h) {
    if (!h || !h->path) { errno = EINVAL; return -1; }
    char tmppath[4096];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", h->path);
    FILE *f = fopen(tmppath, "w");
    if (!f) return -1;

    // Determine slice start to fit max constraint
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

// Load history file from disk into memory, parsing escapes and enforcing max.
int history_load(History *h) {
    if (!h || !h->path) { errno = EINVAL; return -1; }
    FILE *f = fopen(h->path, "r");
    if (!f) return (errno == ENOENT) ? 0 : -1;

    char *line = NULL;
    size_t cap = 0;

    // ssize_t is used by getline() for the length read (including newline if present).
    ssize_t n;

    // Read file line-by-line until EOF.
    while ((n = getline(&line, &cap, f)) != -1) {
        // Strip a single trailing '\n' added by getline(), if present.
        if (n && (line[n-1] == '\n')) line[n-1] = '\0';

        // Parse the expected format: "<epoch>\t<status>\t<cmd>"
        char *p = line;

        // Find first tab separating <epoch> and <status>.
        char *tab1 = strchr(p, '\t');
        if (!tab1) continue;   // Malformed line: skip without failing the load
        *tab1 = '\0';          // Terminate <epoch> field

        // Find second tab separating <status> and <cmd>.
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;   // Malformed line: skip
        *tab2 = '\0';          // Terminate <status> field

        // Convert epoch (base 10). Error handling is lenient: malformed → 0.
        long epoch = strtol(p, NULL, 10);

        // Convert status (base 10). Likewise lenient conversion.
        int status = (int)strtol(tab1 + 1, NULL, 10);

        // Unescape command payload; returns freshly malloc'd string.
        char *cmd_unesc = unescape(tab2 + 1);
        if (!cmd_unesc) {
            // Allocation failure: clean up and abort the load.
            free(line);
            fclose(f);
            return -1;
        }

        // Ensure we have capacity for a new entry; on failure, abort cleanly.
        if (check_cap(h, h->len + 1) != 0) {
            free(cmd_unesc);
            free(line);
            fclose(f);
            return -1;
        }

        // Append a new entry at the end of the array.
        HistEntry *e = &h->v[h->len++];
        e->id = h->next_id++;         // Assign monotonically increasing ID
        e->when = (time_t)epoch;      // Restore timestamp
        e->status = status;           // Restore status
        e->line = cmd_unesc;          // Take ownership of unescaped command buffer
    }

    // getline()-managed buffer: free once after loop.
    free(line);

    // Close the file regardless of parse results; errors already handled above.
    fclose(f);

    // Enforce maximum capacity post-load by dropping oldest entries if needed.
    if (h->len > h->max) {
        size_t drop = h->len - h->max;  // Number of oldest entries to remove

        // Free dynamic payloads for entries being dropped.
        for (size_t i = 0; i < drop; ++i)
            free_entry(&h->v[i]);

        // Slide the surviving entries down to the start.
        // memmove is used (not memcpy) due to overlapping regions.
        memmove(h->v, h->v + drop, (h->len - drop) * sizeof(HistEntry));

        // Update logical length to reflect removed entries.
        h->len -= drop;
    }

    return 0;
}

// Adjust maximum entries immediately, dropping oldest items if over the limit.
// - If max_entries == 0, keep the existing h->max.
// - Returns 0 on success, -1 on invalid args.
int history_stifle(History *h, size_t max_entries) {
    if (!h) { errno = EINVAL; return -1; }

    // Update maximum if a new nonzero limit is provided; otherwise leave unchanged.
    h->max = max_entries ? max_entries : h->max;

    // Already within limit: nothing to do.
    if (h->len <= h->max) return 0;

    // Determine how many oldest entries must be dropped.
    size_t drop = h->len - h->max;

    // Free payloads for the entries that will be removed from the front.
    for (size_t i = 0; i < drop; ++i)
        free_entry(&h->v[i]);

    // Shift remaining entries to the front of the array (handles overlap safely).
    memmove(h->v, h->v + drop, (h->len - drop) * sizeof(HistEntry));

    // Shrink logical length.
    h->len -= drop;

    return 0;
}


// Decide whether a prospective history line should be ignored
// based on flags and content. Returns 1 = ignore, 0 = accept.
static int should_ignore(const History *h, const char *line) {
    if (!line) return 1;  // Null input line → ignore outright

    // Ignore empty/blank lines if HISTORY_IGNORE_EMPTY is set
    if ((h->flags & HISTORY_IGNORE_EMPTY) && is_blank(line))
        return 1;

    // Ignore if HISTORY_IGNORE_SPACE is set AND first char is space
    if ((h->flags & HISTORY_IGNORE_SPACE) && line[0] == ' ') {
        LOG(LOG_LEVEL_INFO, "[ignorespace] len=%zu new=\"%s\"\n",
            h->len, line ? line : "<null>");
        return 1;
    }

    // HISTORY_TRIM_TRAILING: trailing spaces will be trimmed in a copy
    // — ignore decision here is not based solely on trailing spaces.
    if (h->flags & HISTORY_TRIM_TRAILING) {
        // intentionally left blank — trimming happens elsewhere
    }

    // Ignore if duplicate of *last* entry and HISTORY_IGNORE_DUPS is set
    if (h->flags & HISTORY_IGNORE_DUPS) {
        LOG(LOG_LEVEL_INFO, "[dupchk] len=%zu new=\"%s\"\n",
            h->len, line ? line : "<null>");
        if (h->len) {
            // Compare against previous entry's line
            const char *prev = h->v[h->len - 1].line;
            LOG(LOG_LEVEL_INFO, "[dupchk] prev=\"%s\" new=\"%s\"\n",
                prev ? prev : "<null>",
                line ? line : "<null>");
            if (prev && strcmp(prev, line) == 0)
                return 1; // exact match → ignore
        }
    }
    return 0; // No ignore condition matched
}

// Add a new history entry from a given line.
// Returns a HistoryAddResult struct with id and readline sync info.
HistoryAddResult history_add(History *h, const char *line) {
    LOG(LOG_LEVEL_INFO, "adding %s", line);
    HistoryAddResult res = {0, 0};          // Default return: id=0, added_to_readline=0
    if (!h || !line) { errno = EINVAL; return res; }

    // Make a working copy so we can trim or modify before storing
    char *work = xstrdup(line);
    LOG(LOG_LEVEL_INFO, "duping %s", line);
    if (!work) return res;                  // malloc failure → return empty result

    // Optionally trim trailing spaces/tabs/newlines
    if (h->flags & HISTORY_TRIM_TRAILING)
        rtrim_spaces(work);
    LOG(LOG_LEVEL_INFO, "trimming %s", line);

    // Apply all ignore rules to the (possibly trimmed) copy
    if (should_ignore(h, work)) {
        free(work);
        return res;
    }

    // Ensure there’s capacity for one more entry
    if (check_cap(h, h->len + 1) != 0) {
        free(work);
        return res;                         // Allocation failure
    }

    // Populate the new HistEntry struct at end of array
    HistEntry *e = &h->v[h->len++];
    e->id = h->next_id++;                   // Assign unique ID and increment counter
    e->when = time(NULL);                   // Timestamp = now
    e->status = -1;                         // Default/unknown status
    e->line = work;                         // Take ownership of strdup'd buffer

    // Mirror into GNU readline’s in-memory history (if linked)
    extern void add_history(const char *);  // Declared here to avoid header dep
    add_history(e->line);

    // Fill out result info for caller
    res.id = e->id;
    res.added_to_readline = 1;

    // Enforce maximum entry limit immediately (drop oldest if needed)
    history_stifle(h, h->max);

    LOG(LOG_LEVEL_INFO, "returning result");
    return res;
}

// Set status field of entry with matching ID.
// Returns 0 on success, -1 if not found.
int history_set_status_by_id(History *h, uint64_t id, int status) {
    if (!h || id == 0) { errno = EINVAL; return -1; }
    // Reverse iteration: if multiple share ID (shouldn't happen), hit latest first.
    for (size_t i = h->len; i-- > 0; ) {
        if (h->v[i].id == id) {
            h->v[i].status = status;
            return 0;
        }
    }
    errno = ENOENT; // No such entry
    return -1;
}

// Set status of the most recent history entry.
int history_set_status_last(History *h, int status) {
    if (!h || h->len == 0) { errno = EINVAL; return -1; }
    h->v[h->len - 1].status = status;
    return 0;
}

// Derive a default path for the history file based on environment variables.
// On *nix: prefers $XDG_STATE_HOME/thrash/history or $HOME/.thrash_history
// On Windows: LOCALAPPDATA\thrash\history or %HOME%\AppData\Local\thrash\history
int history_default_path(char *out, size_t out_sz) {
    if (!out || out_sz == 0) { errno = EINVAL; return -1; }
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
#ifdef _WIN32
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata &&
        snprintf(out, out_sz, "%s\\thrash\\history", appdata) < (int)out_sz)
        return 0;
    if (home &&
        snprintf(out, out_sz, "%s\\AppData\\Local\\thrash\\history", home) < (int)out_sz)
        return 0;
#else
    if (xdg &&
        snprintf(out, out_sz, "%s/thrash/history", xdg) < (int)out_sz)
        return 0;
    if (home &&
        snprintf(out, out_sz, "%s/.thrash_history", home) < (int)out_sz)
        return 0;
#endif
    errno = ENOENT; // No suitable env var/path found
    return -1;
}