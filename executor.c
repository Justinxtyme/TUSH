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
#include "redirect.h"
#include "command.h"
#include "parser.h"
#include "path.h"
#include <errno.h>
#include <termios.h>    
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

extern char **environ;

// Program prefix for error messages. Consider wiring this to your prompt name.
//static const char *progname = "thrash"; 

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
        /* s truncated — treat as error to avoid partial data */
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
        LOG(LOG_LEVEL_INFO, "unsupported: %c", c);
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




/*Command **parse_commands(const char *input, int *num_cmds) {
    Command **cmds = calloc(MAX_CMDS, sizeof(Command *));
    if (!cmds) {
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }

    int cmd_index = 0;
    int arg_index = 0;
    int buff_index = 0;

    char token_buff[1024];
    bool in_single = false, in_double = false;
    const char *p = input;
    bool aborted = false;

    // Allocate first Command
    Command *current = calloc(1, sizeof(Command));
    if (!current) {
        free(cmds);
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }
    current->argv = calloc(MAX_ARGS, sizeof(char *));
    if (!current->argv) {
        free(current);
        free(cmds);
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }

    while (*p) {
        char c = *p;

        // 1) Escape sequence: copy next char verbatim
        if (c == '\\' && p[1]) {
            if (buff_index < (int)sizeof(token_buff) - 1) {
                token_buff[buff_index++] = p[1];
            }
            p += 2;
            continue;
        }

        // 2) Quote toggles
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            p++;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            p++;
            continue;
        }

        // 3) Pipe: end current command, start next
        if (c == '|' && !in_single && !in_double) {
            // flush any pending word
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                if (arg_index < MAX_ARGS - 1) {
                    current->argv[arg_index++] = strdup(token_buff);
                }
                buff_index = 0;
            }

            // terminate current->argv
            current->argv[arg_index] = NULL;
            current->argc = arg_index;

            // store command
            if (cmd_index < MAX_CMDS) {
                cmds[cmd_index++] = current;
            } else {
                LOG(LOG_LEVEL_ERR, "Too many commands, discarding extra");
                free_command(current);
                aborted = true;
                break;
            }

            // allocate next Command
            current = calloc(1, sizeof(Command));
            if (!current) { aborted = true; break; }
            current->argv = calloc(MAX_ARGS, sizeof(char *));
            if (!current->argv) { free(current); aborted = true; break; }

            arg_index = 0;
            p++;
            continue;
        }

        // 4) Redirection: >, >>, <
        if ((c == '>' || c == '<') && !in_single && !in_double) {
            char chevron = c;
            bool is_append = (c == '>' && p[1] == '>');
            int specified_fd = -1;

            // If the token buffer is all digits, consume it as a FD spec
            if (buff_index > 0) {
                bool all_digits = true;
                for (int i = 0; i < buff_index; i++) {
                    if (token_buff[i] < '0' || token_buff[i] > '9') {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    specified_fd = atoi(token_buff);
                    buff_index = 0;
                } else {
                    // flush it as a real argv word
                    token_buff[buff_index] = '\0';
                    if (arg_index < MAX_ARGS - 1) {
                        current->argv[arg_index++] = strdup(token_buff);
                    }
                    buff_index = 0;
                }
            }

            // skip the chevron(s)
            p += is_append ? 2 : 1;

            // skip spaces before filename
            while (*p && isspace((unsigned char)*p)) p++;

            // parse the filename (respecting quotes)
            buff_index = 0;
            while (*p && (!isspace((unsigned char)*p) || in_single || in_double)) {
                if (*p == '\'' && !in_double) {
                    in_single = !in_single;
                    p++;
                } else if (*p == '"' && !in_single) {
                    in_double = !in_double;
                    p++;
                } else {
                    if (buff_index < (int)sizeof(token_buff) - 1) {
                        token_buff[buff_index++] = *p;
                    }
                    p++;
                }
            }
            token_buff[buff_index] = '\0';
            char *filename = strdup(token_buff);
            buff_index = 0;

            // assign to input/output as appropriate
            if (chevron == '<') {
                current->input_file = filename;
                current->input_fd = (specified_fd != -1) ? specified_fd : 0;
            } else if (chevron == '>' && is_append) {
                current->append_file = filename;
                current->output_fd = (specified_fd != -1) ? specified_fd : 1;
            } else {
                current->output_file = filename;
                current->output_fd = (specified_fd != -1) ? specified_fd : 1;
            }

            // do not push filename to argv
            continue;
        }

        // 5) Whitespace: flush token
        if (isspace((unsigned char)c) && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                if (arg_index < MAX_ARGS - 1) {
                    current->argv[arg_index++] = strdup(token_buff);
                }
                buff_index = 0;
            }
            p++;
            continue;
        }

        // 6) Regular character: accumulate
        if (buff_index < (int)sizeof(token_buff) - 1) {
            token_buff[buff_index++] = c;
        }
        p++;
    }

    // Final flush if not aborted
    if (!aborted) {
        if (buff_index > 0) {
            token_buff[buff_index] = '\0';
            if (arg_index < MAX_ARGS - 1) {
                current->argv[arg_index++] = strdup(token_buff);
            }
        }
        current->argv[arg_index] = NULL;
        current->argc = arg_index;

        if (cmd_index < MAX_CMDS) {
            cmds[cmd_index++] = current;
        } else {
            LOG(LOG_LEVEL_ERR, "Final command exceeds MAX_CMDS, discarding");
            free_command(current);
        }
    }

    if (num_cmds) *num_cmds = cmd_index;
    return cmds;
} */




// A pipe consists of two fds: [0]=read end, [1]=write end.
typedef int pipe_pair_t[2];
/* create_pipes
 * Allocate and initialize num_cmds-1 pipes for a pipeline of num_cmds commands.
 * Returns a calloc’d array of pipe_pair_t, each with CLOEXEC set.
 * On failure closes any fds opened so far, frees the array, and returns NULL. */
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

// Execute a single command with redirection and cwd override.
// This is called in the child process after fork().
static void exec_command(ShellContext *shell, Command *cmd) {
  
    char *resolved_path = NULL;

    if (has_slash(cmd->argv[0])) {
        if (!is_regular(cmd->argv[0]) || !is_executable(cmd->argv[0])) {
            print_exec_error(cmd->argv[0], errno);
            exit(1);
        }
        resolved_path = cmd->argv[0];
    } else {
        if (search_path_alloc(cmd->argv[0], &resolved_path) != 0 || !resolved_path) {
            print_exec_error(cmd->argv[0], ENOENT);
            exit(1);
        }
        if (!is_regular(resolved_path) || !is_executable(resolved_path)) {
            print_exec_error(resolved_path, errno);
            free((void *)resolved_path);
            exit(1);
        }
    }
    
    Redirection *redirs = NULL;
    int redir_count = extract_redirections(cmd, &redirs); // you write this helper

    
    LOG(LOG_LEVEL_INFO, "Performing redirections");
    if (perform_redirections(redirs, redir_count) != 0) {
        if (resolved_path != cmd->argv[0]) free((void *)resolved_path);
        free(redirs);
        exit(1); // early bail in child
    }

    // 🚀 Execute
    execve(resolved_path, cmd->argv, environ);

    // only reached if execve() fails
    print_exec_error(resolved_path, errno);

    if (resolved_path != cmd->argv[0]) free((void *)resolved_path);
    free(redirs);
    _exit(1);
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

static int handle_builtin_in_pipeline(ShellContext *shell, Command *cmd, int num_cmds) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) return 0;

    const char *name = cmd->argv[0];

    if (strcmp(name, "cd") == 0) {
        handle_cd(cmd); // now accepts full Command*
        reclaim_terminal(shell);
        shell->pipeline_pgid = 0;
        return 1; // handled, skip fork
    }

    if (strcmp(name, "exit") == 0) {
        if (num_cmds == 1) {
            shell->running = 0;
            return 2; // signal shell to exit
        } else {
            fprintf(stderr,
                    "thrash: builtin 'exit' cannot be used in a pipeline\n");
            return 1; // handled, skip fork
        }
    }

    return 0; // not a builtin
}


/* Child-side setup: PGID, dup2 pipes, close FDs, reset signals, exec. */
static void setup_pipeline_child(ShellContext *shell, int idx, int num_cmds, pipe_pair_t *pipes, Command *cmd, pid_t leader_pgid) {
    // Process group: leader or join existing group
    if (leader_pgid == 0) {
        setpgid(0, 0); // become group leader
    } else {
        if (setpgid(0, leader_pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM)
        {
            // benign races—parent will retry
        }
    }

    // Wire up stdin from previous pipe
    if (idx > 0) {
        if (dup2(pipes[idx - 1][0], STDIN_FILENO) < 0) {
            perror("dup2 stdin");
            _exit(127);
        }
    }

    // Wire up stdout to next pipe
    if (idx < num_cmds - 1) {
        if (dup2(pipes[idx][1], STDOUT_FILENO) < 0) {
            perror("dup2 stdout");
            _exit(127);
        }
    }

    // Close all pipe FDs (we're done with them)
    if (pipes) {
        for (int j = 0; j < num_cmds - 1; ++j) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }
    }

    // Reset signals to default behavior
    setup_child_signals();

    // Execute the command with redirection and cwd override
    exec_command(shell, cmd);

    // Defensive fallback—should never reach here
    _exit(127);
}



/* ================= Refactored launch_commands ================= */
// Ownership: cmds is BORROWED. launch_commands MUST NOT free or modify cmds or any Command/argv.
// Caller (process_input_segments) frees via free_command_list(cmds, num_cmds) after return.
int launch_commands(ShellContext *shell, Command **cmds, int num_cmds) {
    int i, status = 0, last_exit = 0;
    pid_t pgid = 0;
    shell->pipeline_pgid = 0; // Reset pipeline PGID

    /* Single-command case */
    if (num_cmds == 1) {
        Command *cmd = cmds[0];
        if (!cmd || !cmd->argv || !cmd->argv[0]) {
            return 0; // Empty command
        }
        if (strcmp(cmd->argv[0], "exit") == 0) {
            shell->running = 0;
            return 0;
        }

        // Built-in: cd (no freeing of cmd)
        if (strcmp(cmd->argv[0], "cd") == 0) {
            int cd_res = handle_cd(cmd);
            return cd_res;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            // Child
            setpgid(0, 0);             // Create new process group
            setup_child_signals();      // Reset signal handlers
            exec_command(shell, cmd);   // Exec external command with redirection
            _exit(127);                 // If exec fails
        }

        // Parent
        pgid = pid;
        shell->pipeline_pgid = pgid;

        // Set PGID, ignore benign races
        if (setpgid(pid, pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM) {
            // benign race, ignore
        }

        give_terminal_to_pgid(shell, pgid);

        // Wait for child to finish or stop
        if (waitpid(pgid, &status, WUNTRACED) < 0 && errno != ECHILD) {
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

    /* Multi-stage pipeline */
    pid_t *pids = calloc(num_cmds, sizeof(pid_t));
    if (!pids) {
        perror("calloc pids");
        shell->pipeline_pgid = 0;
        return 1;
    }

    pipe_pair_t *pipes = create_pipes(num_cmds);
    if (num_cmds > 1 && !pipes) {
        perror("pipe setup");
        free(pids);
        shell->pipeline_pgid = 0;
        return 1;
    }

    pid_t last_pid = -1;
    int final_status = 0, got_last = 0;

    for (i = 0; i < num_cmds; ++i) {
        Command *cmd = cmds[i];
        if (!cmd || !cmd->argv || !cmd->argv[0]) {
            LOG(LOG_LEVEL_INFO, "cmds[%d] is NULL or empty", i);
            continue;
        }

        LOG(LOG_LEVEL_INFO, "cmds[%d][0] = '%s'", i, cmd->argv[0]);

        int builtin_res = handle_builtin_in_pipeline(shell, cmd, num_cmds);
        if (builtin_res == 1) continue; // Handled without fork
        if (builtin_res == 2) {
            // Early return (e.g., 'exit' in pipeline semantics)
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            return 0;
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            LOG(LOG_LEVEL_ERR, "fork failed for cmds[%d]", i);
            perror("fork");
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            return 1;
        }

        if (pids[i] == 0) {
            // Child
            pid_t leader = (pgid == 0) ? 0 : pgid;
            setup_pipeline_child(shell, i, num_cmds, pipes, cmd, leader);
            // setup_pipeline_child should exec or _exit on failure
            _exit(127);
        } else {
            // Parent
            if (pgid == 0) {
                pgid = pids[i];
                shell->pipeline_pgid = pgid;
                try_setpgid(pids[i], pgid);
                give_terminal_to_pgid(shell, pgid);
            } else {
                try_setpgid(pids[i], pgid);
            }
        }
    }

    int forked = 0;
    last_pid = -1;
    for (int k = 0; k < num_cmds; ++k) {
        if (pids[k] > 0) {
            ++forked;
            LOG(LOG_LEVEL_INFO, "child %d successfully forked", k + 1);
            last_pid = pids[k];
        } else {
            LOG(LOG_LEVEL_INFO, "child %d failed fork", k + 1);
        }
    }

    int live = forked;
    if (pipes) close_pipes(pipes, num_cmds);

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
            destroy_pipes(pipes, num_cmds);
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
        if (last_pid > 0) {
            int tmp;
            pid_t r = waitpid(last_pid, &tmp, WNOHANG);
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

    destroy_pipes(pipes, num_cmds);
    free(pids);
    shell->pipeline_pgid = 0;

    // Do NOT free cmds or Command here.
    return ret;
}

/*=================================process_input_segments=====================================
processes expanded input, splitting at semicolons for */
void process_input_segments(ShellContext *shell, const char *expanded_input) {
    
    char **segments = split_on_semicolons(expanded_input);
    if (!segments) return;

    for (int i = 0; segments[i]; ++i) {
        int num_cmds = 0;
        Command **cmds = parse_commands(segments[i], &num_cmds);
        LOG(LOG_LEVEL_INFO, "parse_commands returned %d commands", num_cmds);

        // Validate command list before doing anything
        bool valid = true;
        if (!cmds || num_cmds <= 0) {
            valid = false;
        } else {
            for (int j = 0; j < num_cmds; ++j) {
                if (!cmds[j]) {
                    LOG(LOG_LEVEL_ERR, "cmds[%d] is NULL", j);
                    valid = false;
                    break;
                }
                if (!cmds[j]->argv || !cmds[j]->argv[0]) {
                    LOG(LOG_LEVEL_ERR, "cmds[%d] has invalid argv", j);
                    valid = false;
                    break;
                }
            }
        }

        if (!valid) {
            LOG(LOG_LEVEL_WARN, "Skipping invalid command segment: '%s'", segments[i]);
            free_command_list(cmds, num_cmds);
            continue;
        }

        const char *cmd_name = cmds[0]->argv[0];

        // Built-in: exit
        if (strcmp(cmd_name, "exit") == 0) {
            shell->running = 0;
            free_command_list(cmds, num_cmds);
            break;
        }

        // Built-in: unset
        if (strcmp(cmd_name, "unset") == 0) {
            if (num_cmds > 1) {
                fprintf(stderr, "%s: cannot be used in a pipeline\n", cmd_name);
                shell->last_status = 1;
                free_command_list(cmds, num_cmds);
                continue;
            }

            if (!cmds[0]->argv[1]) {
                fprintf(stderr, "unset: missing variable name\n");
                shell->last_status = 1;
                free_command_list(cmds, num_cmds);
                continue;
            }

            LOG(LOG_LEVEL_INFO, "Unsetting variable(s)");
            for (int j = 1; cmds[0]->argv[j]; ++j) {
                LOG(LOG_LEVEL_INFO, "checking variable");
                if (!vart_unset(shell->vars, cmds[0]->argv[j])) {
                    fprintf(stderr, "unset: failed to unset '%s'\n", cmds[0]->argv[j]);
                    shell->last_status = 1;
                }
            }
            shell->last_status = 0;
            free_command_list(cmds, num_cmds);
            continue;
        }

        // Variable assignment
        if (is_var_assignment(cmd_name)) {
            LOG(LOG_LEVEL_INFO, "initiating variable");
            char *eq = strchr(cmd_name, '=');
            size_t name_len = eq - cmd_name;
            char *name = strndup(cmd_name, name_len);
            char *value = strdup(eq + 1);
            vart_set(shell->vars, name, value, 0);
            LOG(LOG_LEVEL_INFO, "%s set to %s", name, value);

            if (num_cmds > 1 && cmds[1] && cmds[1]->argv && cmds[1]->argv[0]) {
                fprintf(stderr, "Setting variable kills pipeline. Killed before (%s %s)\n",
                        cmds[1]->argv[0], cmds[1]->argv[1] ? cmds[1]->argv[1] : "");
            }

            free(name);
            free(value);
            free_command_list(cmds, num_cmds);
            continue;
        }

        LOG(LOG_LEVEL_INFO, "Executing segment: '%s'", segments[i]);
        int status = launch_commands(shell, cmds, num_cmds);
        shell->last_status = status;
        LOG(LOG_LEVEL_INFO, "Segment %d exited with status %d", i, status);

        if (status == 128 + SIGTSTP) {
            add_job(shell->last_pgid, segments[i]);
            fprintf(stderr, "[%d]+  Stopped  %s\n", next_job_id() - 1, segments[i]);
        }
        
        if (num_cmds == 1) { LOG(LOG_LEVEL_INFO, "command exited with %d", status); }  
         else { LOG(LOG_LEVEL_INFO, "pipeline exited with %d", status); } 
    
        free_command_list(cmds, num_cmds);
    }

    free_segments(segments);
}
void free_segments(char **segments) {
    if (!segments) return;
    for (int i = 0; segments[i]; ++i) {
        free(segments[i]);
    }
    free(segments);
}

