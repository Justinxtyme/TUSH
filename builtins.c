#include "builtins.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SHELL_EXIT 42 

int handle_cd(char **args) {
    const char *path = args[1] ? args[1] : getenv("HOME");
    if (chdir(path) != 0) {
        perror("cd");
        return -1;
    }
    return 0;
}

int handle_exit(char **args) {
    return SHELL_EXIT;
}


// add builtins as needed
bool is_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0;
}