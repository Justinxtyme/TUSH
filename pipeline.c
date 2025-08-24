#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>  
#include <errno.h>
#include <time.h>
#include <limits.h>
#include "pipeline.h"
#include "shell.h"
#include "command.h"
#include "executor.h"
#include "signals.h"
#include "builtins.h"


// A pipe consists of two fds: [0]=read end, [1]=write end.
typedef int pipe_pair_t[2];
/* create_pipes
 * Allocate and initialize num_cmds-1 pipes for a pipeline of num_cmds commands.
 * Returns a calloc’d array of pipe_pair_t, each with CLOEXEC set.
 * On failure closes any fds opened so far, frees the array, and returns NULL. */
pipe_pair_t *create_pipes(int num_cmds) {
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


/* Close all pipe FDs in parent */
void close_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    for (int i = 0; i < num_cmds - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Close and free */
void destroy_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    close_pipes(pipes, num_cmds);
    free(pipes);
}


/* Child-side setup: PGID, dup2 pipes, close FDs, reset signals, exec. */
void setup_pipeline_child(ShellContext *shell, int idx, int num_cmds, pipe_pair_t *pipes, Command *cmd, pid_t leader_pgid) {
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

void try_setpgid(pid_t pid, pid_t pgid) {
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

int handle_builtin_in_pipeline(ShellContext *shell, Command *cmd, int num_cmds) {
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