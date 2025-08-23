#include <stdbool.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "path.h"
#include "debug.h"


// Program prefix for error messages. Consider wiring this to your prompt name.
static const char *progname = "thrash"; 


/* has_slash
 * Returns true if the string contains a '/' character.
 * Used to decide whether argv[0] is a path (./a.out, /bin/ls) or a plain command name (ls). */
bool has_slash(const char *s) {
    //return s && strchr(s, '/') != NULL;
    LOG(LOG_LEVEL_INFO, "ENTER has_slash(\"%s\")", s ? s : "(null)");
    bool found = (s && strchr(s, '/') != NULL);
    LOG(LOG_LEVEL_INFO, "  has_slash → %s", found ? "true" : "false");
    return found;
}

/* is_directory
 * stat(2) the path and report whether it's a directory.
 * Returns false on stat errors or when not a directory.  */
bool is_directory(const char *path) {
    struct stat st;
    LOG(LOG_LEVEL_INFO, "ENTER is_directory(\"%s\")", path ? path : "(null)");
    bool rd = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    LOG(LOG_LEVEL_INFO, "  is_directory → %s", rd ? "true" : "false");
    return rd;
}

/* is_regular
 * stat(2) the path and report whether it's a regular file.
 * Returns false on stat errors or when not a regular file.  */
bool is_regular(const char *path) {
    struct stat st;
    //LOG(LOG_LEVEL_INFO, "ENTER is_regular(\"%s\")", path ? path : "(null)");
    bool rg = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    //LOG(LOG_LEVEL_INFO, "  is_regular → %s", rg ? "true" : "false");
    return rg;
}


/* is_executable
 * Uses access(2) with X_OK to check executability for the current user.
 * Note: doesn't confirm file type; combine with is_regular when needed.*/
bool is_executable(const char *path) {
    //LOG(LOG_LEVEL_INFO, "ENTER is_executable(\"%s\")", path ? path : "(null)");
    bool ex = (access(path, X_OK) == 0);
    //LOG(LOG_LEVEL_INFO, "  is_executable → %s", ex ? "true" : "false");
    return ex;
}


/* PATH lookup result codes to disambiguate outcomes without forking.  */
enum path_lookup {
    FOUND_EXEC   = 0,   // Found an executable regular file
    NOT_FOUND    = -1,  // No candidate found anywhere on PATH
    FOUND_NOEXEC = -2,  // Found regular file but not executable
    FOUND_DIR    = -3   // Found a directory named like the command
};

 /* search_path_alloc
 * Resolve a command name (no slash) against PATH, trying each segment.
 *
 * On success (FOUND_EXEC):
 *   - Writes the absolute/relative candidate path into 'out' and returns FOUND_EXEC.
 * On other outcomes:
 *   - Returns NOT_FOUND / FOUND_NOEXEC / FOUND_DIR to allow tailored messages.
 *
 * Notes:
 * - Empty PATH segment means current directory; we render "./cmd" in that case.
 * - We prefer the first executable regular file we encounter.
 * - If we encounter only non-exec files or directories named like the cmd,
 *   we remember that to return a more precise error (126 vs 127). */
 // INTEGRATE WITH DEBUG!
 int search_path_alloc(const char *cmd, char **outp) {
    const char *path = getenv("PATH");
    if (!path || !*path) return NOT_FOUND;

    int found_noexec = 0, found_dir = 0;

    for (const char *p = path; ; ) { // Iterate over each segment of the PATH
        const char *colon = strchr(p, ':'); // Find the next colon
        size_t seg_len = colon ? (size_t)(colon - p) : strlen(p); // Get segment length
        size_t need = (seg_len ? seg_len + 1 : 2) + strlen(cmd) + 1; // Calculate total length needed
        char *candidate = malloc(need); // Allocate memory for the candidate path
        if (!candidate) return NOT_FOUND; // Handle malloc failure

        if (seg_len == 0)  // Current directory
            snprintf(candidate, need, "./%s", cmd); // "./cmd" if empty segment
        else
            snprintf(candidate, need, "%.*s/%s", (int)seg_len, p, cmd); // "<segment>/<cmd>" if not empty

        if (is_directory(candidate)) { 
            found_dir = 1;
            free(candidate);
        } else if (is_regular(candidate)) { 
            if (is_executable(candidate)) {
                *outp = candidate;  // caller takes ownership
                return FOUND_EXEC;
            } else {
                found_noexec = 1; // Remember non-executable regular file
                free(candidate); // Free memory for non-executable regular file
            }
        } else {
            free(candidate);
        }
        if (!colon) break;
        p = colon + 1;
    }
    if (found_noexec) return FOUND_NOEXEC;
    if (found_dir)    return FOUND_DIR;
    return NOT_FOUND;
}

 /* print_exec_error
 * Map common errno values from a failed execve to user-friendly, shell-like errors.
 * This keeps output consistent and avoids raw perror prefixes.*/
 void print_exec_error(const char *what, int err) {
    switch (err) {
        case EACCES:
            fprintf(stderr, "%s: %s: Permission denied\n", progname, what);
            break;
        case ENOEXEC:
            // e.g., binary/text without valid exec header; often 126
            fprintf(stderr, "%s: %s: Exec format error\n", progname, what);
            break;
        case ENOENT:
            // Missing file, or missing shebang interpreter
            fprintf(stderr, "%s: %s: No such file or directory\n", progname, what);
            break;
        case ENOTDIR:
            // A path component wasn't a directory
            fprintf(stderr, "%s: %s: Not a directory\n", progname, what);
            break;
        default:
            // Fallback for rarer cases (E2BIG, ETXTBSY, etc.)
            fprintf(stderr, "%s: %s: %s\n", progname, what, strerror(err));
            break;
    }
}
