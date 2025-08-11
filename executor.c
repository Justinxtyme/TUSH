// executor.c
// TUSH external command execution with clear, shell-like errors and correct exit codes.
//
// Behavior summary:
// - Builtins (cd, exit) handled here and returned directly.
// - Non-builtins:
//   - If argv[0] contains a slash, treat it as a path and diagnose it.
//   - If no slash, search PATH yourself to differentiate:
//       * not found       → "command not found" (exit 127), no fork
//       * found directory → "Is a directory" (exit 126), no fork
//       * found non-exec  → "Permission denied" (exit 126), no fork
//       * found exec file → fork + exec
// - On exec failure, print a clean one-line error and exit with:
//       * 126 when found but not runnable (EACCES, ENOEXEC, directory)
//       * 127 when not found (ENOENT, ENOTDIR path prefix issues)
// This avoids the generic "exec: No such file or directory" for every case.

#include "executor.h"
#include "builtins.h"
#include "debug.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define bool _Bool

extern char **environ;  // Environment passed to execve

// Program prefix for error messages. Consider wiring this to your prompt name.
static const char *progname = "tush"; //

/*
 * has_slash
 * Returns true if the string contains a '/' character.
 * Used to decide whether argv[0] is a path (./a.out, /bin/ls) or a plain command name (ls).
 */
bool has_slash(const char *s) {
    //return s && strchr(s, '/') != NULL;
    LOG(LOG_LEVEL_INFO, "ENTER has_slash(\"%s\")", s ? s : "(null)");
    bool found = (s && strchr(s, '/') != NULL);
    LOG(LOG_LEVEL_INFO, "  has_slash → %s", found ? "true" : "false");
    return found;




}

/*
 * is_directory
 * stat(2) the path and report whether it's a directory.
 * Returns false on stat errors or when not a directory.
 */
bool is_directory(const char *path) {
    struct stat st;
    LOG(LOG_LEVEL_INFO, "ENTER is_directory(\"%s\")", path ? path : "(null)");
    bool rd = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    LOG(LOG_LEVEL_INFO, "  is_directory → %s", rd ? "true" : "false");
    return rd;



}

/*
 * is_regular
 * stat(2) the path and report whether it's a regular file.
 * Returns false on stat errors or when not a regular file.
 */
bool is_regular(const char *path) {
    struct stat st;
    LOG(LOG_LEVEL_INFO, "ENTER is_regular(\"%s\")", path ? path : "(null)");
    bool rg = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    LOG(LOG_LEVEL_INFO, "  is_regular → %s", rg ? "true" : "false");

    return rg;
}

/*
 * is_executable
 * Uses access(2) with X_OK to check executability for the current user.
 * Note: doesn't confirm file type; combine with is_regular when needed.
 */
bool is_executable(const char *path) {
    LOG(LOG_LEVEL_INFO, "ENTER is_executable(\"%s\")", path ? path : "(null)");
    bool ex = (access(path, X_OK) == 0);
    LOG(LOG_LEVEL_INFO, "  is_executable → %s", ex ? "true" : "false");

    return ex;
}

/*
 * PATH lookup result codes to disambiguate outcomes without forking.
 */
enum path_lookup {
    FOUND_EXEC   = 0,   // Found an executable regular file
    NOT_FOUND    = -1,  // No candidate found anywhere on PATH
    FOUND_NOEXEC = -2,  // Found regular file but not executable
    FOUND_DIR    = -3   // Found a directory named like the command
};

/*
 * search_path
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
 *   we remember that to return a more precise error (126 vs 127).
 */

 // INTEGRATE WITH DEBUG!
  // INTEGRATE WITH DEBUG!
   // INTEGRATE WITH DEBUG!
    // INTEGRATE WITH DEBUG!
 int search_path_alloc(const char *cmd, char **outp) {
    const char *path = getenv("PATH");
    if (!path || !*path) return NOT_FOUND;

    int found_noexec = 0, found_dir = 0;

    for (const char *p = path; ; ) {
        const char *colon = strchr(p, ':');
        size_t seg_len = colon ? (size_t)(colon - p) : strlen(p);

        size_t need = (seg_len ? seg_len + 1 : 2) + strlen(cmd) + 1;
        char *candidate = malloc(need);
        if (!candidate) return NOT_FOUND;

        if (seg_len == 0)
            snprintf(candidate, need, "./%s", cmd);
        else
            snprintf(candidate, need, "%.*s/%s", (int)seg_len, p, cmd);

        if (is_directory(candidate)) {
            found_dir = 1;
            free(candidate);
        } else if (is_regular(candidate)) {
            if (is_executable(candidate)) {
                *outp = candidate;  // caller takes ownership
                return FOUND_EXEC;
            } else {
                found_noexec = 1;
                free(candidate);
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
/*
 * print_exec_error
 * Map common errno values from a failed execve to user-friendly, shell-like errors.
 * This keeps output consistent and avoids raw perror prefixes.
 */

 // INTEGRATE WITH DEBUG!
  // INTEGRATE WITH DEBUG!
   // INTEGRATE WITH DEBUG!
    // INTEGRATE WITH DEBUG!
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

/*
 * run_command
 * Entry point from the shell for executing a parsed argv (args).
 * Returns an exit status to store in $? and to influence the prompt, etc.
 *
 * Flow:
 * 1) Handle builtins (cd, exit) immediately.
 * 2) If args[0] has no slash: resolve via PATH and short-circuit non-exec cases (no fork).
 * 3) If args[0] has a slash: treat as a path; pre-diagnose directory and permissions.
 * 4) Fork and exec the decided path. On exec failure, print a clean message and return 126/127.
 * 5) Parent waits and returns child's exit status or signal-based status (128+signum).
 */

 // INTEGRATE WITH DEBUG!
  // INTEGRATE WITH DEBUG!
   // INTEGRATE WITH DEBUG!
    // INTEGRATE WITH DEBUG!
 int run_command(char **args) {
    LOG(LOG_LEVEL_INFO, "ENTER run_command args=%p", (void*)args);
    if (!args || !args[0]) return 0;  // Empty input: no-op, success

    // Builtins first (extend this section as you add more builtins)
    if (strcmp(args[0], "cd") == 0) {
        LOG(LOG_LEVEL_INFO, "builtin cd");
        return handle_cd(args);
    } else if (strcmp(args[0], "exit") == 0) {
        LOG(LOG_LEVEL_INFO, "builtin exit");
        return handle_exit(args);
    }

    const char *cmd = args[0];
    const char *path_to_exec = NULL;
    char *resolved = NULL;  // malloc’d path if resolved via PATH
    LOG(LOG_LEVEL_INFO, "cmd=\"%s\"", cmd);

    if (has_slash(cmd)) {
        LOG(LOG_LEVEL_INFO, "treating \"%s\" as literal path", cmd);
        // Treat argv[0] as a literal path (absolute or relative)
        if (is_directory(cmd)) {
            fprintf(stderr, "%s: %s: Is a directory\n", progname, cmd);
            return 126;
        }
        if (is_regular(cmd) && !is_executable(cmd)) {
            fprintf(stderr, "%s: %s: Permission denied\n", progname, cmd);
            return 126;
        }
        path_to_exec = cmd;
    } else {
        LOG(LOG_LEVEL_INFO, "resolving \"%s\" via PATH", cmd);
        // No slash → resolve via PATH using malloc-based helper
        int r = search_path_alloc(cmd, &resolved);
        if (r == NOT_FOUND) {
            fprintf(stderr, "%s: command not found: %s\n", progname, cmd);
            return 127;
        } else if (r == FOUND_DIR) {
            fprintf(stderr, "%s: %s: Is a directory\n", progname, cmd);
            LOG(LOG_LEVEL_WARN, "search_path_alloc → FOUND_DIR");
            return 126;
        } else if (r == FOUND_NOEXEC) {
            fprintf(stderr, "%s: %s: Permission denied\n", progname, cmd);
            LOG(LOG_LEVEL_WARN, "search_path_alloc → FOUND_NOEXEC");
            return 126;
        } else { // FOUND_EXEC
            path_to_exec = resolved;
            LOG(LOG_LEVEL_INFO, "search_path_alloc → \"%s\"", resolved);
        }
    }

    // At this point we have a candidate path to exec. Fork and run it.
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork failed: %s\n", progname, strerror(errno));
        LOG(LOG_LEVEL_ERR, "fork() failed: %s", strerror(errno));
        if (resolved) free(resolved);
        return 1;
    }

    if (pid == 0) {
        LOG(LOG_LEVEL_INFO, "in child, execve(\"%s\")", path_to_exec);
        execve(path_to_exec, args, environ);

        int err = errno;

        if (is_directory(path_to_exec)) {
            fprintf(stderr, "%s: %s: Is a directory\n", progname, path_to_exec);
            _exit(126);
        }

        print_exec_error(path_to_exec, err);
        int status = (err == EACCES || err == ENOEXEC) ? 126 : 127;
        _exit(status);
    }

    // Parent: wait for child and propagate status
    int wstatus = 0;
    LOG(LOG_LEVEL_INFO, "parent waiting for pid=%d", pid);
    if (waitpid(pid, &wstatus, 0) < 0) {
        LOG(LOG_LEVEL_ERR, "waitpid() failed: %s", strerror(errno));
        fprintf(stderr, "%s: waitpid failed: %s\n", progname, strerror(errno));
        if (resolved) free(resolved);
        return 1;
    }

    if (resolved) free(resolved);  // Clean up malloc’d path

    if (WIFEXITED(wstatus)) {
        LOG(LOG_LEVEL_INFO, "child exited normally with %d", code);
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        LOG(LOG_LEVEL_WARN, "child killed by signal %d", sig);
        return 128 + WTERMSIG(wstatus);
    }
    LOG(LOG_LEVEL_WARN, "run_command reached unexpected exit path");
    return 1;
}





























/*
#include "executor.h"
#include "builtins.h"
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int run_command(char **args) {
    if (args[0] == NULL) return 0;

    if (strcmp(args[0], "cd") == 0) {
        return handle_cd(args);
    } else if (strcmp(args[0], "exit") == 0) {
        return handle_exit(args);
    }

    pid_t pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        perror("exec");
        exit(1);
    } else {
        waitpid(pid, NULL, 0);
    }
    return 0;
} */
