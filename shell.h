// shell.h
#ifndef SHELL_H
#define SHELL_H

#define INPUT_SIZE 1024

typedef struct {
    char input[INPUT_SIZE];   // User input buffer
    int running;              // Shell loop control flag
    char cwd[512];            // Current working directory
    // Add more fields as needed: history, env vars, etc.
} ShellContext;

#endif

