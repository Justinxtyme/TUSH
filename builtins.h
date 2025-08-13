#include <stdbool.h>
#ifndef BUILTINS_H
#define BUILTINS_H
#endif
int handle_cd(char **args);
int handle_exit(char **args);
bool is_builtin(const char *cmd)
