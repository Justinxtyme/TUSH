/* this is an attempt to create a simple shell in C, called "TUSH", Totally Unnecessary Shell, Totally Useless Shell, or The Ultimate Shell.
 it will support basic built-in commands like 'cd', 'exit', and 'ls'   
 and other posix commands */

#include "input.h"
#include "shell.h" // Include the shell context and function declarations
#include "executor.h" // Include the command execution function
#include "jobs.h"
#include "history.h" 
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdio.h> // for printf, fgets, perror
#include <stdlib.h> // for exit,
#include <string.h> // for str maniopulation functions
#include "debug.h"
#include "signals.h"
#include <signal.h>


#ifdef _WIN32
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h> //// for POSIX functions like fork, execvp, chdir
#endif 



// --- Main Loop ---
int main() {
    ShellContext shell = { .running = 1 }; // Initialize shell context with running flag set to 1
    //init_shell(&shell); // Initialize the shell context
    setup_parent_signals();
    setup_shell_job_control(&shell);
    initialize_readline();
    
    //HISTORY SETUP
    char hist_path[4096];
    if (history_default_path(hist_path, sizeof(hist_path)) != 0) {
        // fallback if no $XDG_STATE_HOME / $HOME
        snprintf(hist_path, sizeof(hist_path), "history.txt");
    }

    if (history_init(&shell.history, hist_path, 2000,
                    HISTORY_IGNORE_EMPTY | HISTORY_IGNORE_DUPS | HISTORY_TRIM_TRAILING) != 0) {
        LOG(LOG_LEVEL_ERR, "Failed to init history: %s", strerror(errno));
    }

    if (history_load(&shell.history) != 0) {
        LOG(LOG_LEVEL_WARN, "No existing history loaded: %s", strerror(errno));
    }

    // Mirror into readline so Up-arrow / Ctrl+R see old entries
    for (size_t i = 0; i < history_count(&shell.history); ++i) {
        const HistEntry *e = get_history(&shell.history, i);
        if (e && e->line) add_history(e->line);
    }
    // Optional: limit readline’s in-memory size to match persistent cap
    //stifle_history((int)shell.history.max);
    
    // Log shell startup 
    LOG(LOG_LEVEL_INFO, "TUSH started, pid=%d", getpid());
   
    while (shell.running) {
        //display_prompt(&shell); // Display the shell prompt

        if (!read_input(&shell)) {  
            LOG(LOG_LEVEL_ERR, "read_input failed: %s", strerror(errno));
            perror("readline failed");
            break; // Ctrl+D or error
        }
        if (shell.input[0] != '\0') { // history 
            // Add to persistent + mirror to readline
            HistoryAddResult hr = history_add(&shell.history, shell.input);
            (void)hr; // if unused
        }        
        //add_to_history(&shell, shell.input); // adds input to history, for reuse
        //int num_cmds = 0;
        // Expand variables like $?
        char *expanded = expand_variables(shell.input, shell.last_status);
        if (!expanded) {
            perror("expand_variables");
            continue;
        }
        LOG(LOG_LEVEL_INFO, "Expanded input: '%s'", expanded);

        process_input_segments(&shell, expanded);

        free(expanded);
    }
    
    if (history_save(&shell.history) != 0) {
        LOG(LOG_LEVEL_WARN, "Failed to save history: %s", strerror(errno));
    }
    history_dispose(&shell.history);  // free internal buffers
    cleanup_readline();
    return 0;
}


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Assume these exist elsewhere in the file:
 *   typedef struct {
 *       /* ... */
 *   } ShellContext;
 *
 *   void LOG(int level, const char *fmt, ...);
 *   int handle_cd(char **argv);
 *   void give_terminal_to_pgid(ShellContext*, pid_t);
 *   void reclaim_terminal(ShellContext*);
 *   void setup_child_signals(void);
 *   void exec_child(char **argv);
 *   int is_builtin(const char *name);
 *   /* LOG_LEVEL_* constants exist */
 */

typedef int pipe_pair_t[2];

/* Allocate an array of (num_cmds-1) pipes, each with CLOEXEC */
static pipe_pair_t *create_pipes(int num_cmds) {
    int count = num_cmds - 1;
    if (count <= 0) return NULL;

    pipe_pair_t *pipes = calloc(count, sizeof(pipe_pair_t));
    if (!pipes) {
        perror("calloc pipes");
        return NULL;
    }

    for (int i = 0; i < count; ++i) {
#ifdef HAVE_PIPE2
        if (pipe2(pipes[i], O_CLOEXEC) < 0) {
#else
        if (pipe(pipes[i]) < 0) {
#endif
            perror("pipe");
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return NULL;
        }
#ifndef HAVE_PIPE2
        if (fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC) < 0)
        {
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
static void close_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    for (int i = 0; i < num_cmds - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Close and free (used on error paths before parent closes all FDs) */
static void destroy_pipes(pipe_pair_t *pipes, int num_cmds) {
    if (!pipes) return;
    close_pipes(pipes, num_cmds);
    free(pipes);
}

/* Retry setpgid(pid, pgid) up to 5 times, back off on unexpected errno */
/* Uncomment LOG line to debug rare races */
static void try_setpgid(pid_t pid, pid_t pgid) {
    if (pid <= 0 || pgid <= 0) return;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (setpgid(pid, pgid) == 0) return;
        if (errno == EACCES || errno == EINVAL || errno == EPERM) return;
        usleep(5000);
    }
    /* LOG(LOG_LEVEL_DEBUG,
          "try_setpgid: setpgid(%d,%d) failed after retries: %s",
          (int)pid, (int)pgid, strerror(errno)); */
}

/* Return codes:
 *   0 = not a builtin
 *   1 = handled (parent should continue)
 *   2 = handled + parent should return immediately (exit shell)
 */
static int handle_builtin_in_pipeline(ShellContext *shell,
                                      char **argv,
                                      int num_cmds)
{
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
                    "tush: builtin 'exit' cannot be used in a pipeline\n");
            return 1;
        }
    }
    return 0;
}

/* Child-side setup: PGID, dup2 pipes, close FDs, reset signals, exec. */
static void setup_pipeline_child(int idx,
                                 int num_cmds,
                                 pipe_pair_t *pipes,
                                 char **cmd,
                                 pid_t leader_pgid)
{
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
    exec_child(cmd);
    _exit(127);  /* defensive */
}

/* ================= Refactored launch_pipeline ================= */
int launch_pipeline(ShellContext *shell,
                    char ***cmds,
                    int num_cmds)
{
    int i, status = 0, last_exit = 0;
    pid_t pgid = 0;
    shell->pipeline_pgid = 0;

    /* Single-command case */
    if (num_cmds == 1) {
        char **args = cmds[0];
        if (!args || !args[0]) return 0;
        if (strcmp(args[0], "cd") == 0) return handle_cd(args);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
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
            errno != EACCES && errno != EINVAL && errno != EPERM)
        {
            /* ignore benign races */
        }
        give_terminal_to_pgid(shell, pgid);

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
        if (!cmds[i]) {
            LOG(LOG_LEVEL_INFO, "cmds[%d] is NULL", i);
            continue;
        }
        if (!cmds[i][0]) {
            LOG(LOG_LEVEL_INFO, "cmds[%d][0] is NULL", i);
            continue;
        }
        LOG(LOG_LEVEL_INFO, "cmds[%d][0] = '%s'", i, cmds[i][0]);

        int builtin_res =
            handle_builtin_in_pipeline(shell, cmds[i], num_cmds);
        if (builtin_res == 1) continue;
        if (builtin_res == 2) {
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            return 0;
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            /* Reinsert original per-arg debug safely */
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
            pid_t leader = (pgid == 0) ? 0 : pgid;
            setup_pipeline_child(i, num_cmds, pipes, cmds[i], leader);
        } else {
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

    /* Compute actual forked children count and last_pid safely */
    int forked = 0;
    last_pid = -1;
    for (int k = 0; k < num_cmds; ++k) {
        if (pids[k] > 0) {
            ++forked;
            last_pid = pids[k];
        }
    }
    int live = forked;

    /* Parent must close its copies of the pipe FDs before waiting. */
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
            /* Pipes already closed above; just free the array. */
            free(pipes);
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

    /* Pipes already closed; just free the container. */
    free(pipes);
    free(pids);
    shell->pipeline_pgid = 0;
    return ret;
}
    

