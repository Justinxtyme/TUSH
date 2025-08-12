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
#include <signal.h>

#define MAX_CMDS 16
#define MAX_ARGS 64

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
 * search_path_alloc
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





char ***parse_pipeline(char *input, int *num_cmds) {
    static char **cmds[MAX_CMDS];  // array of command argv[]
    static char *args[MAX_CMDS][MAX_ARGS];  // storage for each argv[]
    int cmd_index = 0;
    int arg_index = 0;

    char *token = strtok(input, " ");
    while (token != NULL) {
        if (strcmp(token, "|") == 0) {
            args[cmd_index][arg_index] = NULL;  // terminate argv
            cmds[cmd_index] = args[cmd_index];  // store argv
            cmd_index++;
            arg_index = 0;
        } else {
            args[cmd_index][arg_index++] = token;
        }
        token = strtok(NULL, " ");
    }

    args[cmd_index][arg_index] = NULL;
    cmds[cmd_index] = args[cmd_index];
    *num_cmds = cmd_index + 1;
    return cmds;
}

// Resolve, validate, set signals, execve, and _exit with correct code
static void exec_child(char **args) {
    char *resolved = NULL;
    const char *path_to_exec;

    // 1) Path lookup
    if (has_slash(args[0])) {
        path_to_exec = args[0];
    } else {
        int r = search_path_alloc(args[0], &resolved);
        if (r == NOT_FOUND)     _exit(127);
        if (r == FOUND_DIR)     _exit(126);
        if (r == FOUND_NOEXEC)  _exit(126);
        path_to_exec = resolved;
    }

    // 2) Type & perm checks
    if (is_directory(path_to_exec))           _exit(126);
    if (is_regular(path_to_exec)
        && !is_executable(path_to_exec))      _exit(126);

    // 3) Reset to default signals in child
    setup_child_signals();

    // 4) Exec—and on failure, pick proper code
    execve(path_to_exec, args, environ);
    int err = errno;
    print_exec_error(path_to_exec, err);
    _exit((err == EACCES || err == ENOEXEC) ? 126 : 127);
}




/* this function launches a pipeline of N commands.
it sets up the necessary pipes and forks child processes to execute each command in the pipeline.
With proper error handling and cleanup. */
int launch_pipeline(char ***cmds, int num_cmds) {
    int i;
    int pipes[num_cmds - 1][2];
    pid_t pids[num_cmds];

    // Single command? handle built‐ins, then fork+exec_child
    if (num_cmds == 1) {
        char **args = cmds[0];
        if (!args || !args[0]) return 0;

        // Built-in dispatch
        if (strcmp(args[0], "cd") == 0)
            return handle_cd(args);

        // Fork + exec
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            exec_child(args);
        }

        // Parent waits and returns exit code/signal
        int w;
        waitpid(pid, &w, 0);
        if (WIFEXITED(w))   return WEXITSTATUS(w);
        if (WIFSIGNALED(w)) return 128 + WTERMSIG(w);
        return 1;
    }

    // Multi‐stage pipeline: create pipes
    for (i = 0; i < num_cmds - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return -1;
        }
    }

    // Fork each stage
    for (i = 0; i < num_cmds; ++i) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            return -1;
        }
        if (pids[i] == 0) {
            // Child: wire up pipes
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < num_cmds - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            // Close all ends
            for (int j = 0; j < num_cmds - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // Exec this stage
            exec_child(cmds[i]);
        }
    }

    // Parent closes all pipe fds
    for (i = 0; i < num_cmds - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // Wait all children, capture last exit
    int last_exit = 0;
    for (i = 0; i < num_cmds; ++i) {
        int w;
        if (waitpid(pids[i], &w, 0) < 0) {
            perror("waitpid");
            continue;
        }
        if (i == num_cmds - 1) {
            if (WIFEXITED(w))   last_exit = WEXITSTATUS(w);
            else if (WIFSIGNALED(w))
                last_exit = 128 + WTERMSIG(w);
        }
    }
    return last_exit;
}























