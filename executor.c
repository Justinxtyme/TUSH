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
#include "pipeline.h"
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


// Execute a single command with redirection and cwd override.
// This is called in the child process after fork().
void exec_command(ShellContext *shell, Command *cmd) {
  
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

