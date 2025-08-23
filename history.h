#ifndef HISTORY_H
#define HISTORY_H
//#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HistEntry {
    uint64_t id;        // Monotonic per-session id (not persisted as index)
    time_t   when;      // Time added
    int      status;    // Exit status (or -1 if unknown)
    char    *line;      // Command string (heap-owned)
} HistEntry;

typedef struct History {
    HistEntry *v;       // Dynamic array
    size_t     len;     // Used length
    size_t     cap;     // Capacity
    size_t     max;     // Max entries (cap and on-disk cap)
    uint64_t   next_id; // Next id to assign
    int        flags;   // Behavior flags
    char      *path;    // Persist file path (heap-owned)
} History;

enum HistoryFlags {
    HISTORY_IGNORE_EMPTY   = 1 << 0, // ignore empty/whitespace-only
    HISTORY_IGNORE_SPACE   = 1 << 1, // ignore commands starting with space
    HISTORY_IGNORE_DUPS    = 1 << 2, // ignore consecutive duplicates
    HISTORY_TRIM_TRAILING  = 1 << 3  // trim trailing spaces
};

typedef struct HistoryAddResult {
    uint64_t id;  // 0 if not added due to filtering
    int      added_to_readline; // boolean
} HistoryAddResult;

// Initialization and lifecycle
int  history_init(History *h, const char *path, size_t max_entries, int flags);
void history_dispose(History *h); // frees all allocations

// Persistence (our format)
int  history_load(History *h);           // load from h->path (no readline side-effects)
int  history_save(const History *h);     // save to h->path (atomic)

// Add / update
HistoryAddResult history_add(History *h, const char *line); // adds now, status unknown
int  history_set_status_by_id(History *h, uint64_t id, int status);
int  history_set_status_last(History *h, int status);

// Query
size_t          history_count(const History *h);
const HistEntry*get_history(const History *h, size_t idx); // 0..len-1

// Utilities
int  history_stifle(History *h, size_t max_entries); // trims if needed
int  history_default_path(char *out, size_t out_sz); // resolves $XDG_STATE_HOME/$HOME

#ifdef __cplusplus
}
#endif // __cplusplus

#endif //HISTORY_H