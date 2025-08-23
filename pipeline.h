#ifndef PIPELINE_H
#define PIPELINE_H

#include <unistd.h>
#include <stdio.h>
#include "command.h"
#include "shell.h"
typedef int pipe_pair_t[2];

pipe_pair_t *create_pipes(int num_cmds);
void close_pipes(pipe_pair_t *pipes, int num_cmds);
void destroy_pipes(pipe_pair_t *pipes, int num_cmds);
void setup_pipeline_child(ShellContext *shell, int idx, int num_cmds, pipe_pair_t *pipes, Command *cmd, pid_t leader_pgid);
void try_setpgid(pid_t pid, pid_t pgid);

#endif