/* =========================================== executor.c ======================================================
 thrash command execution and pipeline orchestration with Bash-like semantics.

 Responsibilities:
 - Builtins (e.g., cd) handled directly and returned without forking.
 - External commands:
   - If argv[0] contains a slash, treat as a path and validate directly.
   - If no slash, search $PATH manually to distinguish:
       * not found       → "command not found" (exit 127), no fork
       * found directory → "is a directory" (exit 126), no fork
       * found non-exec  → "permission denied" (exit 126), no fork
       * found exec file → fork + exec

       - On exec failure, print a clean one-line error and exit with:
       * 126 for non-runnable files (EACCES, ENOEXEC, directory)
       * 127 for missing files (ENOENT, ENOTDIR)
 - 
 Pipeline execution:
 - Allocates pipes and pids dynamically (no VLAs).
 - Forks each stage, wires stdin/stdout via dup2(), and sets CLOEXEC on pipe fds.
 - Assigns a shared process group (PGID) for job control.
 - Handles terminal handoff and reclaiming via tcsetpgrp().
 - Tracks last command’s PID to return accurate pipeline exit status.
 - Cleans up all fds and memory on every exit path.

 Signal handling:
 - Shell ignores SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU.
 - Children restore default signals before exec.
 - Optional SIGCHLD blocking is available to prevent early reaping.

 This file ensures robust, race-tolerant execution with clear diagnostics and correct exit codes. */

#include "executor.h"
#include "builtins.h"
#include "debug.h"
#include "signals.h"
#include "shell.h"
#include "jobs.h"
#include "input.h"
#include "var.h"
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
#include <ctype.h>  
#include <errno.h>
#include <time.h>
#include <limits.h>

#define MAX_CMDS 16
#define MAX_ARGS 64

//#define bool _Bool

// Program prefix for error messages. Consider wiring this to your prompt name.
static const char *progname = "thrash"; //

char *expand_variables(const char *input, int last_exit) {
    // If input is NULL, return NULL immediately
    if (!input) return NULL;

    const char *needle = "$?"; // Target variable to expand (represents last exit status)
    char *pos = strstr(input, needle); 
    if (!pos) { 
        // If "$?" is not found in input, return a duplicate of the original string
        return strdup(input);
    }

    // Convert last_exit integer to string (e.g., 1 → "1")
    char exit_str[16];
    snprintf(exit_str, sizeof(exit_str), "%d", last_exit);

    // Calculate lengths for allocation
    size_t input_len = strlen(input);
    size_t result_len = input_len + 16;  // Add extra space for expansion (safe bufffer)
    char *result = calloc(1, result_len); // Allocate zero-initialized memory
    if (!result) return NULL; // Bail out if allocation fails

    const char *src = input; // Source pointer for reading input
    char *dst = result;      // Destination pointer for writing output

    // Iterate through input string
    while (*src) {
        // If we find "$?" pattern
        if (src[0] == '$' && src[1] == '?') {
            strcpy(dst, exit_str);        // Copy exit status string to result
            dst += strlen(exit_str);      // Advance destination pointer
            src += 2;                     // Skip over "$?" in source
        } else {
            *dst++ = *src++;              // Copy character as-is
        }
    }

    *dst = '\0'; // Null-terminate the result string
    return result; // Return the expanded string
}


/* Growable buffer helpers */
static int ensure_cap(char **buf, size_t *cap, size_t min_needed, char **cursor) {
    size_t used = (size_t)(*cursor - *buf);
    if (min_needed <= *cap) return 1;
    size_t new_cap = *cap ? *cap : 64;
    while (new_cap < min_needed) {
        if (new_cap > SIZE_MAX / 2) new_cap = SIZE_MAX;
        else new_cap *= 2;
        if (new_cap < min_needed) return 0;
    }
    char *nbuf = realloc(*buf, new_cap);
    if (!nbuf) return 0;
    *buf = nbuf;
    *cap = new_cap;
    *cursor = nbuf + used;
    return 1;
}

static int append_mem(char **buf, size_t *cap, char **cursor, const void *src, size_t n) {
    size_t used = (size_t)(*cursor - *buf);
    if (!ensure_cap(buf, cap, used + n + 1, cursor)) return 0;
    if (n) memcpy(*cursor, src, n);
    *cursor += n;
    **cursor = '\0';
    return 1;
}

static inline int append_ch(char **buf, size_t *cap, char **cursor, char c) {
    return append_mem(buf, cap, cursor, &c, 1);
}
/* ... keep your ensure_cap/append_mem/append_ch helpers above ... */
/* Expand variables:
 *  - $?      -> last_exit
 *  - $NAME   -> lookup via vart_get()
 *  - ${NAME} -> lookup; if missing `}` emit literal "${" + rest
 *  - \$      -> literal $
 *
 * Returns malloc'd string (caller frees) or NULL on OOM/error.
 */
char *expand_variables_ex(const char *input, int last_exit, const VarTable *vars) {
    if (!input) return NULL;

    char exit_str[16];
    int exit_len = snprintf(exit_str, sizeof(exit_str), "%d", last_exit);
    if (exit_len < 0) return NULL;
    if (exit_len >= (int)sizeof(exit_str)) {
        /* snprintf would have truncated — treat as error to avoid partial data */
        return NULL;
    }
    LOG(LOG_LEVEL_INFO, "string=%s", exit_str);
    char *out = NULL;
    size_t cap = 0;
    char *dst = NULL;

    if (!ensure_cap(&out, &cap, 64, &dst)) return NULL;
    *dst = '\0';
    const char *src = input;
    while (*src) {
        /* Escaped dollar: \$  -> emit literal '$' (drop backslash) */
        if (src[0] == '\\' && src[1] == '$') {
            if (!append_ch(&out, &cap, &dst, '$')) goto oom;
            src += 2;
            continue;
        }

        if (*src != '$') {
            if (!append_ch(&out, &cap, &dst, *src++)) goto oom;
            continue;
        }

        /* We have a '$' */
        src++; /* consume '$' */

        /* Case: $? */
        if (*src == '?') {
            if (!append_mem(&out, &cap, &dst, exit_str, (size_t)exit_len)) goto oom;
            src++;
            continue;
        }

        /* Case: ${NAME} */
        if (*src == '{') {
            const char *name_start = ++src; /* skip '{' */
            const char *scan = name_start;
            while (*scan && *scan != '}') scan++;
            if (*scan != '}') {
                /* No closing brace: emit literal "${" and reprocess rest literally */
                if (!append_mem(&out, &cap, &dst, "${", 2)) goto oom;
                src = name_start; /* reprocess rest literally */
                continue;
            }
            size_t name_len = (size_t)(scan - name_start);
            if (name_len == 0) {
                /* Empty name -> literal ${} */
                if (!append_mem(&out, &cap, &dst, "${}", 3)) goto oom;
                src = scan + 1;
                continue;
            }

            char name[256];
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';

            const char *val = "";
            if (vars) {
                Var *v = vart_get(vars, name);
                if (v && v->value) val = v->value;
            }
            if (!append_mem(&out, &cap, &dst, val, strlen(val))) goto oom;
            src = scan + 1; /* skip '}' */
            continue;
        }

        /* Case: $NAME where NAME = [A-Za-z_][A-Za-z0-9_]* */
        unsigned char c = (unsigned char)*src;
        if (isalpha(c) || c == '_') {
            const char *name_start = src;
            src++;
            while (*src) {
                unsigned char d = (unsigned char)*src;
                if (isalnum(d) || d == '_') src++;
                else break;
            }
            size_t name_len = (size_t)(src - name_start);
            char name[256];
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';

            const char *val = "";
            if (vars) {
                Var *v = vart_get(vars, name);
                if (v && v->value) val = v->value;
            }
            if (!append_mem(&out, &cap, &dst, val, strlen(val))) goto oom;
            continue;
        }
        LOG(LOG_LEVEL_INFO, "line 248 check");
        /* Unsupported/positional/ lone '$' -> emit literal '$' and reprocess next char */
        if (!append_ch(&out, &cap, &dst, '$')) goto oom;
        /* do not advance src here; next loop will handle current char */
    }
    *dst = '\0';
    LOG(LOG_LEVEL_INFO, "returning %s", out);
    return out;

oom:
    free(out);
    return NULL;
}

//extern char **environ;  // Environment passed to execve


/* has_slash
 * Returns true if the string contains a '/' character.
 * Used to decide whether argv[0] is a path (./a.out, /bin/ls) or a plain command name (ls). */
bool has_slash(const char *s) {
    //return s && strchr(s, '/') != NULL;
    //LOG(LOG_LEVEL_INFO, "ENTER has_slash(\"%s\")", s ? s : "(null)");
    bool found = (s && strchr(s, '/') != NULL);
    //LOG(LOG_LEVEL_INFO, "  has_slash → %s", found ? "true" : "false");
    return found;
}

/* is_directory
 * stat(2) the path and report whether it's a directory.
 * Returns false on stat errors or when not a directory.  */
bool is_directory(const char *path) {
    struct stat st;
    //LOG(LOG_LEVEL_INFO, "ENTER is_directory(\"%s\")", path ? path : "(null)");
    bool rd = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    //LOG(LOG_LEVEL_INFO, "  is_directory → %s", rd ? "true" : "false");
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

char ***parse_pipeline(const char *input, int *num_cmds) {
    char ***cmds = calloc(MAX_CMDS, sizeof(char **));
    if (!cmds) return NULL;

    int cmd_index = 0;
    int arg_index = 0;
    char **argv = calloc(MAX_ARGS, sizeof(char *));
    if (!argv) {
        free(cmds);
        return NULL;
    }

    const char *p = input;
    char token_buff[1024];
    int buff_index = 0;
    bool in_single = false, in_double = false;

    while (*p) {
        char c = *p;

        if (c == '\\' && p[1]) {
            token_buff[buff_index++] = p[1];
            p += 2;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
            p++;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
            p++;
        } else if (c == '|' && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                argv[arg_index++] = strdup(token_buff);
                buff_index = 0;
            }
            argv[arg_index] = NULL;
            cmds[cmd_index++] = argv;
            argv = calloc(MAX_ARGS, sizeof(char *));
            arg_index = 0;
            p++;
        } else if (isspace(c) && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                argv[arg_index++] = strdup(token_buff);
                buff_index = 0;
            }
            p++;
        } else {
            token_buff[buff_index++] = c;
            p++;
        }
    }

    if (buff_index > 0) {
        token_buff[buff_index] = '\0';
        argv[arg_index++] = strdup(token_buff);
    }

    argv[arg_index] = NULL;
    cmds[cmd_index++] = argv;
    *num_cmds = cmd_index;

    return cmds;
}


// A pipe consists of two fds: [0]=read end, [1]=write end.
typedef int pipe_pair_t[2];
/* create_pipes
 * Allocate and initialize num_cmds-1 pipes for a pipeline of num_cmds commands.
 * Returns a calloc’d array of pipe_pair_t, each with CLOEXEC set.
 * On failure closes any fds opened so far, frees the array, and returns NULL. */
/*static pipe_pair_t *create_pipes(int num_cmds) {
    int count = num_cmds - 1;
    if (count <= 0) {
        return NULL;
    }

    pipe_pair_t *pipes = calloc(count, sizeof(pipe_pair_t));
    if (!pipes) {
        return NULL;
    }

    for (int i = 0; i < count; ++i) {
    #if defined(HAVE_PIPE2)
        if (pipe2(pipes[i], O_CLOEXEC) < 0) {
    #else
            if (pipe(pipes[i]) < 0) {
    #endif
                // tear down what we built so far
                for (int j = 0; j < i; ++j) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                free(pipes);
                return NULL;
            }
    #ifndef HAVE_PIPE2
            if (fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC) < 0 ||
                fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC) < 0) {
                for (int j = 0; j <= i; ++j) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                free(pipes);
                return NULL;
            }
    #endif
        }
    return pipes;     
    }
*/

static pipe_pair_t *create_pipes(int num_cmds) {
    int count = num_cmds - 1;
    if (count <= 0) {
        return NULL;
    }

    pipe_pair_t *pipes = calloc(count, sizeof(pipe_pair_t));
    if (!pipes) {
        return NULL;
    }

    for (int i = 0; i < count; ++i) {
#if defined(HAVE_PIPE2)
        if (pipe2(pipes[i], O_CLOEXEC) < 0) {
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return NULL;
        }
#else
        if (pipe(pipes[i]) < 0) {
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return NULL;
        }
        if (fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC) < 0) {
            for (int j = 0; j <= i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return NULL;
        }
#endif
    }

    return pipes;
}

//=====================================EXEC_CHILD================================================
// used for executing child processes, including resolving paths and setting up the environment
// args: command arguments, including argv[0] (the command to execute)  
// Resolve, validate, set signals, execve, and _exit with correct code
// ==============================================================================================
static void exec_child(ShellContext *ctx, char **args) {
    char *resolved = NULL;
    const char *path_to_exec;

    // 1) Path lookup
    if (has_slash(args[0])) {
        path_to_exec = args[0];
    } else {
        int r = search_path_alloc(args[0], &resolved);
        if (r == NOT_FOUND) {
            fprintf(stderr, "thrash: command not found: %s\n", args[0]);
            _exit(127);
        }
        if (r == FOUND_DIR) {
            fprintf(stderr, "thrash: is a directory: %s\n", args[0]);
            _exit(126);
        }
        if (r == FOUND_NOEXEC) {
            fprintf(stderr, "thrash: permission denied: %s\n", args[0]);
            _exit(126);
        }
        path_to_exec = resolved;
    }

    // 2) Type & perm checks
    if (is_directory(path_to_exec)) {
        fprintf(stderr, "thrash: is a directory: %s\n", path_to_exec);
        _exit(126);
    }
    if (is_regular(path_to_exec) && !is_executable(path_to_exec)) {
        fprintf(stderr, "thrash: permission denied: %s\n", path_to_exec);
        _exit(126);
    }

    // 3) Reset to default signals in child
    setup_child_signals();

    // 4) Exec — on failure, pick proper code and message
    //execve(path_to_exec, args, environ);
    char **envp = vart_build_envp(ctx->vars);  // your shell's exported vars
    execve(path_to_exec, args, envp);          // use your envp, not environ
    //if error
    int err = errno;
    vart_free_envp(envp);
    if (resolved) free(resolved);
    if (err == ENOEXEC) {
        fprintf(stderr, "thrash: exec format error: %s\n", path_to_exec);        
        _exit(126);
    } else if (err == EACCES) {
        fprintf(stderr, "thrash: permission denied: %s\n", path_to_exec);
        _exit(126);
    } else if (err == ENOENT) {
        // Prefer the original argv[0] for 'not found'
        fprintf(stderr, "thrash: command not found: %s\n", args[0]);
        _exit(127);
    } else {
        fprintf(stderr, "thrash: failed to exec %s: %s\n", path_to_exec, strerror(err));
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

/* Close all pipe FDs in parent */
static void close_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    for (int i = 0; i < num_cmds - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Close and free */
static void destroy_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    close_pipes(pipes, num_cmds);
    free(pipes);
}

static void try_setpgid(pid_t pid, pid_t pgid) {
    if (pid <= 0 || pgid <= 0) return;

    struct timespec delay = {0, 5 * 1000 * 1000}; // 5ms

    for (int attempt = 0; attempt < 10; ++attempt) {
        if (setpgid(pid, pgid) == 0) return;

        switch (errno) {
            case EACCES:
            case EINVAL:
            case EPERM:
            case ESRCH:
                return; // Fatal or process gone
            default:
                // Transient error—retry
                break;
        }

        nanosleep(&delay, NULL);
    }
    // Optional: log persistent failure
    fprintf(stderr, "try_setpgid: failed to setpgid(%d, %d): %s\n",
            pid, pgid, strerror(errno));
}

static int handle_builtin_in_pipeline(ShellContext *shell, char **argv, int num_cmds) {                                      
    if (!argv || !argv[0]) return 0;

    if (strcmp(argv[0], "cd") == 0) {
        handle_cd(argv);
        reclaim_terminal(shell);
        shell->pipeline_pgid = 0;
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0) {
        if (num_cmds == 1) {
            shell->running = 0;
            return 2;
        } else {
            fprintf(stderr,
                    "thrash: builtin 'exit' cannot be used in a pipeline\n");
            return 1;
        }
    }
    return 0;
}

/* Child-side setup: PGID, dup2 pipes, close FDs, reset signals, exec. */
static void setup_pipeline_child(ShellContext *shell, int idx, int num_cmds, pipe_pair_t *pipes, char **cmd, pid_t leader_pgid) { 
    /* Process group: leader or join existing group */
    if (leader_pgid == 0) {
        setpgid(0, 0);
    } else {
        if (setpgid(0, leader_pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM)
        {
            /* ignore benign races; parent will retry */
        }
    }

    /* Wire up stdin/stdout */
    if (idx > 0) {
        if (dup2(pipes[idx - 1][0], STDIN_FILENO) < 0) _exit(127);
    }
    if (idx < num_cmds - 1) {
        if (dup2(pipes[idx][1], STDOUT_FILENO) < 0) _exit(127);
    }

    /* Close all pipe FDs */
    if (pipes) {
        for (int j = 0; j < num_cmds - 1; ++j) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }
    }

    /* Reset signals and exec */
    setup_child_signals();
    exec_child(shell, cmd);
    _exit(127);  /* defensive */
}


/* ================= Refactored launch_pipeline ================= */
int launch_pipeline(ShellContext *shell, char ***cmds, int num_cmds) {
    int i, status = 0, last_exit = 0;
    pid_t pgid = 0;
    shell->pipeline_pgid = 0; // Reset pipeline PGID

    /* Single-command case */
    if (num_cmds == 1) {
        char **args = cmds[0];
        if (!args || !args[0]) return 0; // Empty command

        // Handle built-in 'cd' directly
        if (strcmp(args[0], "cd") == 0) return handle_cd(args);

        pid_t pid = fork(); // Fork child process
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            setpgid(0, 0);               // Create new process group
            setup_child_signals();      // Reset signal handlers
            exec_child(shell, args);           // Exec external command
            _exit(127);                 // Failsafe exit if exec fails
        }

        pgid = pid;
        shell->pipeline_pgid = pgid;

        // Set PGID, ignore benign races
        if (setpgid(pid, pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM)
        {
            // benign race, ignore
        }

        give_terminal_to_pgid(shell, pgid); // Give terminal to child

        // Wait for child to finish or stop
        if (waitpid(pgid, &status, WUNTRACED) < 0 && errno != ECHILD) {
            perror("waitpid");
        }

        if (WIFSTOPPED(status)) {
            reclaim_terminal(shell);        // Reclaim terminal on stop
            shell->last_pgid = pgid;        // Save PGID for job tracking
            shell->pipeline_pgid = 0;
            return 128 + WSTOPSIG(status);  // Return stop signal code
        }
        if (WIFEXITED(status))        last_exit = WEXITSTATUS(status);   // Normal exit
        else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status); // Killed by signal

        reclaim_terminal(shell);
        shell->pipeline_pgid = 0;
        return last_exit; // Return final exit status
    }

    /* Multi-stage pipeline */
    pid_t *pids = calloc(num_cmds, sizeof(pid_t)); // Allocate PID array
    if (!pids) {
        perror("calloc pids");
        shell->pipeline_pgid = 0;
        return 1;
    }

    pipe_pair_t *pipes = create_pipes(num_cmds); // Create pipe pairs
    if (num_cmds > 1 && !pipes) {
        perror("pipe setup");
        free(pids);
        shell->pipeline_pgid = 0;
        return 1;
    }

    pid_t last_pid = -1;
    int final_status = 0, got_last = 0;

    for (i = 0; i < num_cmds; ++i) {
        if (!cmds[i]) {
            LOG(LOG_LEVEL_INFO, "cmds[%d] is NULL", i);
            continue;
        }
        if (!cmds[i][0]) {
            LOG(LOG_LEVEL_INFO, "cmds[%d][0] is NULL", i);
            continue;
        }
        LOG(LOG_LEVEL_INFO, "cmds[%d][0] = '%s'", i, cmds[i][0]);

        // Handle built-ins inside pipeline
        int builtin_res = handle_builtin_in_pipeline(shell, cmds[i], num_cmds);
        if (builtin_res == 1) continue; // Skip fork
        if (builtin_res == 2) {
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            return 0;
        }

        pids[i] = fork(); // Fork pipeline stage
        if (pids[i] < 0) {
            // Log args for debugging
            if (cmds[i]) {
                for (int k = 0; cmds[i][k]; ++k) {
                    LOG(LOG_LEVEL_INFO, "cmds[%d][%d] = '%s'", i, k, cmds[i][k]);
                }
            }
            perror("fork");
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            return 1;
        }

        if (pids[i] == 0) {
            pid_t leader = (pgid == 0) ? 0 : pgid; // First child becomes group leader
            setup_pipeline_child(i, num_cmds, pipes, cmds[i], leader); // Setup redirections and exec
        } else {
            if (pgid == 0) {
                pgid = pids[i]; // First child sets PGID
                shell->pipeline_pgid = pgid;
                try_setpgid(pids[i], pgid);
                give_terminal_to_pgid(shell, pgid); // Give terminal to pipeline
            } else {
                try_setpgid(pids[i], pgid); // Join existing PGID
            }
        }
    }

    /* Compute actual forked children count and last_pid safely */
    int forked = 0;
    last_pid = -1;
    for (int k = 0; k < num_cmds; ++k) {
        if (pids[k] > 0) {
            ++forked;
            last_pid = pids[k]; // Track last valid PID
        }
    }
    int live = forked;

    if (pipes) close_pipes(pipes, num_cmds); // Close pipe fds in parent

    // Wait for all children in the pipeline
    while (live > 0) {
        pid_t w = waitpid(-pgid, &status, WUNTRACED); // Wait for any child in PGID
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
            destroy_pipes(pipes, num_cmds);
            free(pids);
            return 128 + WSTOPSIG(status); // Return stop signal code
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            --live;
            if (WIFEXITED(status))        last_exit = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);
            if (w == last_pid) {
                final_status = status;
                got_last = 1; // Mark final command status
            }
        }
    }

    reclaim_terminal(shell); // Restore terminal to shell
    shell->last_pgid = pgid;

    int ret;
    if (got_last) {
        if (WIFEXITED(final_status))        ret = WEXITSTATUS(final_status);
        else if (WIFSIGNALED(final_status)) ret = 128 + WTERMSIG(final_status);
        else                                ret = last_exit;
    } else {
        if (last_pid > 0) {
            int tmp;
            pid_t r = waitpid(last_pid, &tmp, WNOHANG); // Try to get last PID status
            if (r == last_pid) {
                if (WIFEXITED(tmp))        ret = WEXITSTATUS(tmp);
                else if (WIFSIGNALED(tmp)) ret = 128 + WTERMSIG(tmp);
                else                       ret = last_exit;
            } else {
                ret = last_exit;
            }
        } else {
            ret = last_exit;
        }
    }

    destroy_pipes(pipes, num_cmds); // Cleanup
    free(pids);
    shell->pipeline_pgid = 0;
    return ret; // Return final status
}

/*=================================process_input_segments=====================================
processes expanded input, splitting at semicolons for */
void process_input_segments(ShellContext *shell, const char *expanded_input) {
    
    // Split the input string into segments using semicolons as delimiters
    char **segments = split_on_semicolons(expanded_input);
    if (!segments) return; // If splitting fails, bail out

    // Iterate over each segment (e.g., "ls -l", "echo hi", etc.)
    for (int i = 0; segments[i]; ++i) {
        int num_cmds = 0;

        // Parse the segment into a pipeline of commands
        char ***cmds = parse_pipeline(segments[i], &num_cmds);
        LOG(LOG_LEVEL_INFO, "parse_pipeline returned %d commands", num_cmds);

        // Skip empty or invalid pipelines
        if (num_cmds == 0 || !cmds || !cmds[0]) continue;

        // Check for built-in "exit" command in the first command of the pipeline
        if (cmds[0][0] && strcmp(cmds[0][0], "exit") == 0) {
            shell->running = 0; // Signal shell to stop running
            break;              // Exit the loop immediately
        }
        
        //unset variables 
        if (strcmp(cmds[0][0], "unset") == 0) {
            if (num_cmds > 1) {
                fprintf(stderr, "%s: cannot be used in a pipeline\n", cmds[0][0]);
                shell->last_status = 1;
                continue;
            } 
            if (!cmds[0][1]) {    // if no argument was given
                fprintf(stderr, "unset: missing variable name\n"); 
                shell->last_status = 1; 
                continue;
            }
            LOG(LOG_LEVEL_INFO, "Unsetting variable(s)");
            for (int i = 1; cmds[0][i]; i++) {
                if (!vart_unset(shell->vars, cmds[0][i])) {
                    fprintf(stderr, "unset: failed to unset '%s'\n", cmds[0][i]);
                    shell->last_status = 1;
                    continue;
                }
            }
            shell->last_status = 0;
            continue;
        }        
        
        if (is_var_assignment(cmds[0][0])) {
            LOG(LOG_LEVEL_INFO, "initiating variable");
            char *eq = strchr(cmds[0][0], '=');
            size_t name_len = eq - cmds[0][0];
            char *name = strndup(cmds[0][0], name_len);
            char *value = strdup(eq + 1);
            vart_set(shell->vars, name, value, 0); // or V_EXPORT if needed
            free(name);
            free(value);
            continue; // skip launching pipeline
        }


        LOG(LOG_LEVEL_INFO, "Executing segment: '%s'", segments[i]);

        // Launch the pipeline and capture its exit status
        int status = launch_pipeline(shell, cmds, num_cmds);
        shell->last_status = status; // Save status for $? expansion
        LOG(LOG_LEVEL_INFO, "Segment %d exited with status %d", i, status);

        // If the pipeline was stopped (e.g., via Ctrl-Z), register it as a job
        if (status == 128 + SIGTSTP) {
            add_job(shell->last_pgid, segments[i]); // Track job for fg/bg
            fprintf(stderr, "[%d]+  Stopped  %s\n", next_job_id()-1, segments[i]);
        }

        LOG(LOG_LEVEL_INFO, "pipeline exited/stopped with %d", status);

        // Free memory allocated for the pipeline commands
        for (int j = 0; j < num_cmds; ++j) {
            for (int k = 0; cmds[j][k]; ++k) {
                free(cmds[j][k]); // Free each argument
            }
            free(cmds[j]); // Free each command array
        }
        free(cmds); // Free the top-level command list
    }

    // Free the array of input segments
    free_segments(segments);
}

void free_segments(char **segments) {
    if (!segments) return;
    for (int i = 0; segments[i]; ++i) {
        free(segments[i]);
    }
    free(segments);
}

