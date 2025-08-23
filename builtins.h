#ifndef BUILTINS_H
#define BUILTINS_H

#include "redirect.h"
#include <stdbool.h>

int handle_cd(Command *cmd);

int handle_exit();

bool is_builtin(const char *cmd);

#endif
