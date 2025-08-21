#ifndef INPUT_H
#define INPUT_H

#include "shell.h" // for ShellContext
#include <stdbool.h>
#include "redirect.h"

int read_input(ShellContext *ctx); // replaces fgets-based version
void initialize_readline(void);
void cleanup_readline(void);
bool is_numeric(const char *s);
bool handle_literal_expansion(ShellContext *shell, Command *cmd);
char **split_on_semicolons(const char *input);

#endif