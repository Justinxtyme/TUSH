#include "shell.h"
#include <stdlib.h>
#include <string.h>


void init_shell(ShellContext *ctx) { // Initialize the shell context
    ctx->history_capacity = 10; // Initial capacity for history
    ctx->history_size = 0; // Start with no commands in history
    ctx->history = malloc(ctx->history_capacity * sizeof(char *)); // Allocate memory for history array
}


void add_to_history(ShellContext *ctx, const char *input) { // Add command to history
    if (ctx->history_size < ctx->history_capacity) {  // If there's space in history
        ctx->history[ctx->history_size++] = strdup(input); // Duplicate the input string and store it
    }
}

