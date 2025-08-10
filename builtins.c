#include "builtins.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int handle_cd(char **args) {
    const char *path = args[1] ? args[1] : getenv("HOME");
    if (chdir(path) != 0) {
        perror("cd");
        return -1;
    }
    return 0;
}

int handle_exit(char **args) {
    exit(0);
}