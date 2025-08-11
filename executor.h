#include <stdbool.h>

#ifndef EXECUTOR_H
#define EXECUTOR_H

int run_command(char **args);
static int search_path_alloc(const char *cmd, char **outp);
static bool has_slash(const char *s); 
static bool is_directory(const char *path);
static bool is_regular(const char *path);
static bool is_executable(const char *path);

#endif  // EXECUTOR_H