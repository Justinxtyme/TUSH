#include <stdbool.h>   // for bool
#include <stddef.h>    // for size_t (if used in future)
#include <unistd.h>    // for execvp, fork, pipe, dup2
#include <sys/types.h> // for pid_t
#include <sys/stat.h>  // for stat, S_ISDIR, etc.
#include <errno.h>     // for errno values
#include <stdlib.h>    // for exit
#include <string.h>    // for strcmp, strtok
#include <stdio.h>
#include "shell.h"
#include "redirect.h"
#ifndef EXECUTOR_H
#define EXECUTOR_H

//int run_command(char **args);
//Command **parse_commands(const char *input, int *num_cmds);

int launch_commands(ShellContext *shell, Command **cmds, int num_cmds);

char *expand_variables_ex(const char *input, int last_exit, const VarTable *vars);


void free_segments(char **segments);

void process_input_segments(ShellContext *shell, const char *expanded_input);

#endif  // EXECUTOR_H