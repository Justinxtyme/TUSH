#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "input.h"
#include <unistd.h> // for getcwd

void initialize_readline(void) {
    rl_bind_key('\t', rl_complete); // optional: enable tab completion
}

void cleanup_readline(void) {
    // optional: add write_history() here if you want persistent history
}

int read_input(ShellContext *ctx) {
    getcwd(ctx->cwd, sizeof(ctx->cwd) - 8); // update cwd for prompt
    char prompt[512];
    snprintf(prompt, sizeof(prompt), "TUSH %.502s: ", ctx->cwd); // %.502s limits to 502 chars to avoid overflow

    char *line = readline(prompt);
    if (!line) return 0; // Ctrl+D or EOF

    if (*line) add_history(line); // non-empty input gets saved

    strncpy(ctx->input, line, sizeof(ctx->input) - 1);
    ctx->input[sizeof(ctx->input) - 1] = '\0'; // ensure null-termination

    free(line); // readline allocates with malloc
    return 1;
}