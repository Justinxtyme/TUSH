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
#include "signals.h"
#include "shell.h"
#include <errno.h>
#include <termios.h>    
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
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
        if (r == NOT_FOUND) {
            fprintf(stderr, "tush: command not found: %s\n", args[0]);
            _exit(127);
        }
        if (r == FOUND_DIR) {
            fprintf(stderr, "tush: is a directory: %s\n", args[0]);
            _exit(126);
        }
        if (r == FOUND_NOEXEC) {
            fprintf(stderr, "tush: permission denied: %s\n", args[0]);
            _exit(126);
        }
        path_to_exec = resolved;
    }

    // 2) Type & perm checks
    if (is_directory(path_to_exec)) {
        fprintf(stderr, "tush: is a directory: %s\n", path_to_exec);
        _exit(126);
    }
    if (is_regular(path_to_exec) && !is_executable(path_to_exec)) {
        fprintf(stderr, "tush: permission denied: %s\n", path_to_exec);
        _exit(126);
    }

    // 3) Reset to default signals in child
    setup_child_signals();

    // 4) Exec — on failure, pick proper code and message
    execve(path_to_exec, args, environ);
    int err = errno;
    if (err == ENOEXEC) {
        fprintf(stderr, "tush: exec format error: %s\n", path_to_exec);
        _exit(126);
    } else if (err == EACCES) {
        fprintf(stderr, "tush: permission denied: %s\n", path_to_exec);
        _exit(126);
    } else if (err == ENOENT) {
        // Prefer the original argv[0] for 'not found'
        fprintf(stderr, "tush: command not found: %s\n", args[0]);
        _exit(127);
    } else {
        fprintf(stderr, "tush: failed to exec %s: %s\n", path_to_exec, strerror(err));
        _exit(126);
    }
}




static void give_terminal_to_pgid(ShellContext *shell, pid_t pgid) {
    // With SIGTTOU ignored, tcsetpgrp won’t stop us if we happen to be bg.
    if (tcsetpgrp(shell->tty_fd, pgid) < 0) {
        // Don’t spam; log if you have a debug flag
        // perror("tcsetpgrp(give)");
    }
}

static void reclaim_terminal(ShellContext *shell) {
    if (tcsetpgrp(shell->tty_fd, shell->shell_pgid) < 0) {
        // perror("tcsetpgrp(reclaim)");
    }
}



int launch_pipeline(ShellContext *shell, char ***cmds, int num_cmds) {
    int i;
    int status = 0, last_exit = 0;
    pid_t pgid = 0;

    if (num_cmds == 1) {
        char **args = cmds[0];
        if (!args || !args[0]) return 0;
        if (strcmp(args[0], "cd") == 0)
            return handle_cd(args);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            // CHILD: put self in new group, restore signals, exec
            // Only set PGID; DO NOT tcsetpgrp here.
            setpgid(0, 0);
            setup_child_signals();
            exec_child(args);
        }

        // PARENT: establish job control
        pgid = pid;
        // Ensure child is the group leader; race-proof by retrying if needed
        if (setpgid(pid, pgid) < 0 && errno != EACCES && errno != EINVAL) {
            // EACCES can happen if child already exec’d and set it; ignore benign failures.
            // EINVAL can occur transiently; you could retry, but usually safe to continue.
        }

        give_terminal_to_pgid(shell, pgid);

        // Wait for child to exit or stop
        if (waitpid(-pgid, &status, WUNTRACED) < 0 && errno != ECHILD) {
            perror("waitpid");
        }

        if (WIFSTOPPED(status)) {
            reclaim_terminal(shell);
            shell->last_pgid = pgid; // remember stopped job’s PGID for fg/bg
            fprintf(stderr, "\n[Stopped]\n");
            return 128 + WSTOPSIG(status);
        }

        if (WIFEXITED(status))      last_exit = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);

        reclaim_terminal(shell);
        return last_exit;
    }

    // ----- Multi-stage pipeline -----
    int pipes[num_cmds - 1][2];
    pid_t pids[num_cmds];

    // 1) Create pipes
    for (i = 0; i < num_cmds - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    // 2) Fork each stage
    for (i = 0; i < num_cmds; ++i) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            return 1;
        }

        if (pids[i] == 0) {
            // CHILD
            // a) Process group: first child becomes leader, others do not touch tcsetpgrp
            if (i == 0) {
                setpgid(0, 0); // self as leader
            } else {
                // Do not rely on a non-shared pgid variable; parent will set our PGID.
                // Optionally call setpgid(0, getppid()) is wrong; just skip.
            }

            // b) Wire pipes
            if (i > 0)                  dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < num_cmds - 1)       dup2(pipes[i][1],     STDOUT_FILENO);

            // c) Close all fds
            for (int j = 0; j < num_cmds - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // d) Restore default signals and exec
            setup_child_signals();
            exec_child(cmds[i]);
        } else {
            // PARENT
            if (i == 0) {
                pgid = pids[0];
                for (int attempt = 0; attempt < 5; ++attempt) {
                     if (setpgid(pids[0], pgid) == 0) break;
                     if (errno == EACCES || errno == EINVAL) break;
                     usleep(1000); // small delay to let child setpgid itself
                     }
                     give_terminal_to_pgid(shell, pgid);
            } else {
                if (setpgid(pids[i], pgid) < 0 && errno != EACCES && errno != EINVAL) {
                    // ignore benign races
                }
            }
        }
    }

    // 3) Parent closes all pipe fds
    for (i = 0; i < num_cmds - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // 4) Wait until the entire pipeline exits or any process stops
    int live = num_cmds;
    while (live > 0) {
        pid_t w = waitpid(-pgid, &status, WUNTRACED);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            perror("waitpid");
            break;
        }

        if (WIFSTOPPED(status)) {
            reclaim_terminal(shell);
            shell->last_pgid = pgid;
            fprintf(stderr, "\n[Pipeline stopped]\n");
            return 128 + WSTOPSIG(status);
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            --live;
            if (WIFEXITED(status))      last_exit = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);
        }
    }

    // 5) Reclaim terminal and store last pgid
    reclaim_terminal(shell);
    shell->last_pgid = pgid;
    return last_exit;
}
