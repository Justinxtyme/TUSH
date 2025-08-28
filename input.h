#ifndef INPUT_H
#define INPUT_H

#include "shell.h" // for ShellContext
#include <stdbool.h>
#include "redirect.h"

int read_input(ShellContext *ctx, bool continuation); 

void initialize_readline(void);

void cleanup_readline(void);

bool is_numeric(const char *s);

bool handle_literal_expansion(ShellContext *shell, Command *cmd);

char **split_on_semicolons(const char *input);

void append_to_buffer(char **buf, const char *chunk);

void free_buffer(char **buf);

bool is_command_complete(const char *cmd);

#endif