#ifndef INPUT_H
#define INPUT_H

#include "shell.h" // for ShellContext

int read_input(ShellContext *ctx); // replaces fgets-based version
void initialize_readline(void);
void cleanup_readline(void);

#endif