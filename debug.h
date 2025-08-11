#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <time.h>

// ─────────────────────────────────────────────────────────────
// Log level definitions
// ─────────────────────────────────────────────────────────────
#define LOG_LEVEL_INFO 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_ERR  3

// Minimum log level to emit (override at compile time with -DLOG_LEVEL_MIN)
#ifndef LOG_LEVEL_MIN
#define LOG_LEVEL_MIN LOG_LEVEL_INFO
#endif

// ─────────────────────────────────────────────────────────────
// ANSI color codes for terminal output
// ─────────────────────────────────────────────────────────────
#define COLOR_RESET "\x1b[0m"
#define COLOR_INFO  "\x1b[36m"  // Cyan
#define COLOR_WARN  "\x1b[33m"  // Yellow
#define COLOR_ERR   "\x1b[31m"  // Red

// ─────────────────────────────────────────────────────────────
// Helper: current time as "HH:MM:SS"
// ─────────────────────────────────────────────────────────────
static inline const char* debug_timestamp(void) {
    static char buf[9]; // "HH:MM:SS"
    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info)) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        snprintf(buf, sizeof(buf), "??:??:??");
    }
    return buf;
}

// ─────────────────────────────────────────────────────────────
// Main logging macro
//
// Emits logs only when DEBUG is defined and
// `level >= LOG_LEVEL_MIN`.
// Format:
//   [timestamp] <COLOR>[LEVEL]</COLOR> file.c:123 (func): your message
// ─────────────────────────────────────────────────────────────
#ifdef DEBUG
#define LOG(level, fmt, ...)                                                        \
    do {                                                                             \
        if ((level) >= LOG_LEVEL_MIN) {                                              \
            const char *color, *label;                                               \
            switch (level) {                                                         \
                case LOG_LEVEL_WARN: color = COLOR_WARN; label = "WARN"; break;      \
                case LOG_LEVEL_ERR:  color = COLOR_ERR;  label = "ERROR"; break;     \
                case LOG_LEVEL_INFO:                                                  \
                default:         color = COLOR_INFO; label = "INFO"; break;         \
            }                                                                        \
            fprintf(stderr,                                                          \
                    "%s %s[%s]%s %s:%d (%s): " fmt "%s\n",                           \
                    debug_timestamp(),                                              \
                    color, label, COLOR_RESET,                                      \
                    __FILE__, __LINE__, __func__,                                    \
                    ##__VA_ARGS__, COLOR_RESET);                                     \
        }                                                                            \
    } while (0)
#else
#define LOG(level, fmt, ...) do {} while (0)
#endif

#endif // DEBUG_H