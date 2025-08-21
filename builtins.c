#include "builtins.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "redirect.h"

#define SHELL_EXIT 42 

int handle_cd(Command *cmd) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) return -1;

    // Use second argument as path, or fallback to $HOME
    const char *path = cmd->argv[1] ? cmd->argv[1] : getenv("HOME");
    if (!path) {
        fprintf(stderr, "thrash: cd: no path and $HOME not set\n");
        return -1;
    }

    if (chdir(path) != 0) {
        perror("cd");
        return -1;
    }

    return 0;
}

int handle_exit() {
    return SHELL_EXIT;
}


// add builtins as needed
bool is_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "export") == 0;;
}