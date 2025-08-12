#include <stdbool.h>   // for bool
#include <stddef.h>    // for size_t (if used in future)
#include <unistd.h>    // for execvp, fork, pipe, dup2
#include <sys/types.h> // for pid_t
#include <sys/stat.h>  // for stat, S_ISDIR, etc.
#include <errno.h>     // for errno values
#include <stdlib.h>    // for exit
#include <string.h>    // for strcmp, strtok
#include <stdio.h>

#ifndef EXECUTOR_H
#define EXECUTOR_H

//int run_command(char **args);
char ***parse_pipeline(char *input, int *num_cmds);

int launch_pipeline(char ***cmds, int num_cmds);

int search_path_alloc(const char *cmd, char **outp);

bool has_slash(const char *s); 

bool is_directory(const char *path);

bool is_regular(const char *path);

bool is_executable(const char *path);

void print_exec_error(const char *what, int err);

#endif  // EXECUTOR_H