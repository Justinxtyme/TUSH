#include "shell.h"
#include "redirect.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <signal.h> 
//#include <unistd.h> // for pid_t
int launch_commands(ShellContext *shell, Command **cmds, int num_cmds) {
    int i, status = 0, last_exit = 0;
    pid_t pgid = 0;
    shell->pipeline_pgid = 0; // Reset pipeline PGID

    /* Single-command case */
    if (num_cmds == 1) {
        Command *cmd = cmds[0];
        if (!cmd || !cmd->argv || !cmd->argv[0]) {
            free_command(cmd);
            return 0; // Empty command
        }

        // Handle built-in 'cd' directly
        if (strcmp(cmd->argv[0], "cd") == 0) {
            int cd_res = handle_cd(cmd->argv);
            free_command(cmd);
            return cd_res;
        }

        pid_t pid = fork(); // Fork child process
        if (pid < 0) {
            perror("fork");
            free_command(cmd);
            return 1;
        }

        if (pid == 0) {
            setpgid(0, 0);               // Create new process group
            setup_child_signals();      // Reset signal handlers
            exec_command(shell, cmd);   // Exec external command with redirection
            _exit(127);                 // Failsafe exit if exec fails
        }

        pgid = pid;
        shell->pipeline_pgid = pgid;

        // Set PGID, ignore benign races
        if (setpgid(pid, pgid) < 0 &&
            errno != EACCES && errno != EINVAL && errno != EPERM) {
            // benign race, ignore
        }

        give_terminal_to_pgid(shell, pgid); // Give terminal to child

        // Wait for child to finish or stop
        if (waitpid(pgid, &status, WUNTRACED) < 0 && errno != ECHILD) {
            perror("waitpid");
        }

        if (WIFSTOPPED(status)) {
            reclaim_terminal(shell);
            shell->last_pgid = pgid;
            shell->pipeline_pgid = 0;
            free_command(cmd);
            return 128 + WSTOPSIG(status);
        }

        if (WIFEXITED(status))        last_exit = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last_exit = 128 + WTERMSIG(status);

        reclaim_terminal(shell);
        shell->pipeline_pgid = 0;
        free_command(cmd);
        return last_exit;
    }

    /* Multi-stage pipeline */
    pid_t *pids = calloc(num_cmds, sizeof(pid_t));
    if (!pids) {
        perror("calloc pids");
        shell->pipeline_pgid = 0;
        for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
        free(cmds);
        return 1;
    }

    pipe_pair_t *pipes = create_pipes(num_cmds);
    if (num_cmds > 1 && !pipes) {
        perror("pipe setup");
        free(pids);
        shell->pipeline_pgid = 0;
        for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
        free(cmds);
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

        int builtin_res = handle_builtin_in_pipeline(shell, cmd->argv, num_cmds);
        if (builtin_res == 1) continue; // Skip fork
        if (builtin_res == 2) {
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
            free(cmds);
            return 0;
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            LOG(LOG_LEVEL_INFO, "fork failed for cmds[%d]", i);
            perror("fork");
            destroy_pipes(pipes, num_cmds);
            free(pids);
            shell->pipeline_pgid = 0;
            for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
            free(cmds);
            return 1;
        }

        if (pids[i] == 0) {
            pid_t leader = (pgid == 0) ? 0 : pgid;
            setup_pipeline_child(shell, i, num_cmds, pipes, cmd, leader);
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
            for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
            free(cmds);
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

    for (int j = 0; j < num_cmds; ++j) free_command(cmds[j]);
    free(cmds);

    return ret;
}


void process_input_segments(ShellContext *shell, const char *expanded_input) {
    // Split the input string into segments using semicolons as delimiters
    char **segments = split_on_semicolons(expanded_input);
    if (!segments) return;

    for (int i = 0; segments[i]; ++i) {
        int num_cmds = 0;

        // Parse the segment into a pipeline of Command structs
        Command **cmds = parse_commands(segments[i], &num_cmds);
        LOG(LOG_LEVEL_INFO, "parse_commands returned %d commands", num_cmds);

        if (num_cmds == 0 || !cmds || !cmds[0] || !cmds[0]->argv || !cmds[0]->argv[0]) {
            continue;
        }

        // Handle built-in "exit"
        if (strcmp(cmds[0]->argv[0], "exit") == 0) {
            shell->running = 0;
            break;
        }

        // Handle "unset"
        if (strcmp(cmds[0]->argv[0], "unset") == 0) {
            if (num_cmds > 1) {
                fprintf(stderr, "%s: cannot be used in a pipeline\n", cmds[0]->argv[0]);
                shell->last_status = 1;
                continue;
            }
            if (!cmds[0]->argv[1]) {
                fprintf(stderr, "unset: missing variable name\n");
                shell->last_status = 1;
                continue;
            }
            LOG(LOG_LEVEL_INFO, "Unsetting variable(s)");
            for (int j = 1; cmds[0]->argv[j]; j++) {
                LOG(LOG_LEVEL_INFO, "checking variable");
                if (!vart_unset(shell->vars, cmds[0]->argv[j])) {
                    fprintf(stderr, "unset: failed to unset '%s'\n", cmds[0]->argv[j]);
                    shell->last_status = 1;
                    continue;
                }
            }
            shell->last_status = 0;
            continue;
        }

        // Handle variable assignment
        if (is_var_assignment(cmds[0]->argv[0])) {
            LOG(LOG_LEVEL_INFO, "initiating variable");
            char *eq = strchr(cmds[0]->argv[0], '=');
            size_t name_len = eq - cmds[0]->argv[0];
            char *name = strndup(cmds[0]->argv[0], name_len);
            char *value = strdup(eq + 1);
            vart_set(shell->vars, name, value, 0);
            LOG(LOG_LEVEL_INFO, "%s set to %s", name, value);
            if (num_cmds > 1 && cmds[1] && cmds[1]->argv && cmds[1]->argv[0]) {
                fprintf(stderr, "Setting variable kills pipeline. Killed before (%s %s)\n",
                        cmds[1]->argv[0], cmds[1]->argv[1] ? cmds[1]->argv[1] : "");
            }
            free(name);
            free(value);
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

        LOG(LOG_LEVEL_INFO, "pipeline exited/stopped with %d", status);

        // Free each Command struct
        for (int j = 0; j < num_cmds; ++j) {
            free_command(cmds[j]); // assumes you have a helper that frees argv, redirection fields, etc.
        }
        free(cmds); // Free the array of Command pointers
    }

    free_segments(segments);
}

