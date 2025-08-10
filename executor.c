#include "executor.h"
#include "builtins.h"
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int run_command(char **args) {
    if (args[0] == NULL) return 0;

    if (strcmp(args[0], "cd") == 0) {
        return handle_cd(args);
    } else if (strcmp(args[0], "exit") == 0) {
        return handle_exit(args);
    }

    pid_t pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        perror("exec");
        exit(1);
    } else {
        waitpid(pid, NULL, 0);
    }
    return 0;
}