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
#include <fcntl.h>  

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

static void try_setpgid(pid_t pid, pid_t pgid) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (setpgid(pid, pgid) == 0) return;
        if (errno == EACCES || errno == EINVAL || errno == EPERM) return;
        usleep(5000);
    }
}




int launch_pipeline(ShellContext *shell, char ***cmds, int num_cmds) {
    int i;
    int status = 0, last_exit = 0;
    pid_t pgid = 0;

    shell->pipeline_pgid = 0; // reset at start

    // Defensive SIGCHLD block to prevent async reaper from stealing statuses
    // Currently not needed — no SIGCHLD handler exists. Uncomment if you add
    // background job monitoring or async waitpid() calls elsewhere.
    /*
    sigset_t block, oldmask;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &oldmask);
    // ... fork + setpgid sequence ...
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    */

    if (num_cmds == 1) {
        char **args = cmds[0];
        if (!args || !args[0]) {
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return 0;
        }
        if (strcmp(args[0], "cd") == 0) {
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return handle_cd(args);
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return 1;
        }

        if (pid == 0) {
            setpgid(0, 0);
            setup_child_signals();
            exec_child(args);
            _exit(127);
        }

        pgid = pid;
        shell->pipeline_pgid = pgid;

        if (setpgid(pid, pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM) {
            // ignore benign races
        }

        give_terminal_to_pgid(shell, pgid);
        //sigprocmask(SIG_SETMASK, &oldmask, NULL);

        if (waitpid(-pgid, &status, WUNTRACED) < 0 && errno != ECHILD) {
            perror("waitpid");
        }

        if (WIFSTOPPED(status)) {
            reclaim_terminal(shell);
            shell->last_pgid = pgid;
            shell->pipeline_pgid = 0;
            return 128 + WSTOPSIG(status);
        }

        if (WIFEXITED(status))        last_exit = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);

        reclaim_terminal(shell);
        shell->pipeline_pgid = 0;
        return last_exit;
    }

    pid_t *pids = calloc(num_cmds, sizeof(pid_t));
    if (!pids) {
        perror("calloc pids");
        //sigprocmask(SIG_SETMASK, &oldmask, NULL);
        shell->pipeline_pgid = 0;
        return 1;
    }

    int (*pipes)[2] = NULL;
    if (num_cmds > 1) {
        pipes = calloc(num_cmds - 1, sizeof(int[2]));
        if (!pipes) {
            perror("calloc pipes");
            free(pids);
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            shell->pipeline_pgid = 0;
            return 1;
        }
    }

    // Create pipes with CLOEXEC for safety
    for (i = 0; i < num_cmds - 1; ++i) {
    #ifdef HAVE_PIPE2
        if (pipe2(pipes[i], O_CLOEXEC) < 0)
    #else
        if (pipe(pipes[i]) < 0)
    #endif
        {
            perror("pipe");
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            free(pids);
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            shell->pipeline_pgid = 0;
            return 1;
        }
    #ifndef HAVE_PIPE2
        fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC);
    #endif
    }

    pid_t last_pid = -1;
    int final_status = 0, got_last = 0;

    for (i = 0; i < num_cmds; ++i) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            if (pipes) {
                for (int j = 0; j < num_cmds - 1; ++j) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                free(pipes);
            }
            free(pids);
            //sigprocmask(SIG_SETMASK, &oldmask, NULL);
            shell->pipeline_pgid = 0;
            return 1;
        }

        if (pids[i] == 0) {
            if (i == 0) {
                setpgid(0, 0);
            } else {
                pid_t target = shell->pipeline_pgid;
                if (target > 0 &&
                    setpgid(0, target) < 0 &&
                    errno != EACCES && errno != EINVAL && errno != EPERM) {
                    // ignore benign races
                }
            }

            // Wire pipes with error checks
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) _exit(127);
            }
            if (i < num_cmds - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) _exit(127);
            }

            if (pipes) {
                for (int j = 0; j < num_cmds - 1; ++j) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
            }

            setup_child_signals();
            exec_child(cmds[i]);
            _exit(127);
        } else {
            if (i == 0) {
                pgid = pids[0];
                shell->pipeline_pgid = pgid;

                try_setpgid(pids[i], pgid);

                give_terminal_to_pgid(shell, pgid);
            } else {
                try_setpgid(pids[i], pgid);
            }
        }
    }

    last_pid = pids[num_cmds - 1];

    if (pipes) {
        for (i = 0; i < num_cmds - 1; ++i) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
    }

    // Restore signal mask before waiting
    //sigprocmask(SIG_SETMASK, &oldmask, NULL);

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
            shell->pipeline_pgid = 0;
            if (pipes) free(pipes);
            free(pids);
            return 128 + WSTOPSIG(status);
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            --live;
            if (WIFEXITED(status))        last_exit = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);

            if (w == last_pid) {
                final_status = status;
                got_last = 1;
            }
        }
    }

    reclaim_terminal(shell);
    shell->last_pgid = pgid;

    int ret;
    if (got_last) {
        if (WIFEXITED(final_status))        ret = WEXITSTATUS(final_status);
        else if (WIFSIGNALED(final_status)) ret = 128 + WTERMSIG(final_status);
        else                                ret = last_exit;
    } else {
        int tmp;
        pid_t r = waitpid(last_pid, &tmp, WNOHANG);
        if (r == last_pid) {
            if (WIFEXITED(tmp))        ret = WEXITSTATUS(tmp);
            else if (WIFSIGNALED(tmp)) ret = 128 + WTERMSIG(tmp);
            else                       ret = last_exit;
        } else {
            ret = last_exit;
        }
    }

    if (pipes) free(pipes);
    free(pids);
    shell->pipeline_pgid = 0; // reset shared handoff
    return ret;
}

