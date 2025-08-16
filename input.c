// input.c 
/* This file implements input handling for the thrash shell
 It uses the readline library for enhanced user input features */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <readline/readline.h> 
#include <readline/history.h> 
#include "input.h" // 
#include "executor.h"
#include <unistd.h> // for getcwd
#include "debug.h"

#define COLOR_THRASH  "\x1b[32m" // green
#define STYLE_RESET "\x1b[0m" 
#define COLOR_CTX "\x1b[35m" // magenta
#define BOLD "\x1b[1m" // bold/bright
#define REVERSE "\x1b[4m"
/*Initialize readline library. This function sets up readline for input handling
 It can be used to enable features like command history and line editing */
void initialize_readline(void) {
    rl_bind_key('\t', rl_complete); // optional: enable tab completion
}

/* This function can be used to free any resources allocated by readline. It is called at the end of the shell session to ensure no memory leaks
 It can also save the command history to a file if desired, For example, you can use write_history("history.txt") to save the history
 // This is optional and can be customized based on your needs */
void cleanup_readline(void) {
    // optional: add write_history() here if you want persistent history
}

/* This function reads input from the user using readline, which provides line editing capabilities
 It returns 1 on success and 0 on EOF or error. The input is stored in the ShellContext's input buffer
 It also updates the current working directory in the ShellContext for prompt display */
int read_input(ShellContext *ctx) { 
    getcwd(ctx->cwd, sizeof(ctx->cwd)); // update cwd for prompt
    char prompt[512]; //
    const char *sh_color = COLOR_THRASH;
    const char *ctx_color = COLOR_CTX;
    //snprintf(prompt, sizeof(prompt), "%s%s%sTHRASH%s) %s%s%.450s%s: ", //ORIGINAL
    //snprintf(prompt, sizeof(prompt), "\001%s%s%s\002THRASH\001%s\002) \001%s%s\002%.450s\001%s\002: ",
    snprintf(prompt, sizeof(prompt), "\001\033[38;2;158;96;3mTHRASH)\033[0m\n\002 \001\033[38;2;158;96;3m%.450s\033[0m\n\002: ",
     //sh_color, BOLD, REVERSE, STYLE_RESET, ctx_color, BOLD, ctx->cwd, STYLE_RESET); // %.502s limits to 502 chars to avoid overflow //ORIGINAL

    char *line = readline(prompt);
    if (!line) return 0; // Ctrl+D or EOF

    //if (*line) add_history(line); // non-empty input gets saved

    strncpy(ctx->input, line, sizeof(ctx->input) - 1);
    ctx->input[sizeof(ctx->input) - 1] = '\0'; // ensure null-termination

    free(line); // readline allocates with malloc
    return 1;
}

bool is_numeric(const char *s) {
    if (!s) return false;

    // Skip leading whitespace
    while (isspace((unsigned char)*s)) s++;

    if (!*s) return false; // empty after trimming

    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }

    return true;
}

bool handle_literal_expansion(ShellContext *shell, const char *expanded) {
    if (!is_numeric(expanded)) {
        return false;
    }

    LOG(LOG_LEVEL_INFO, "Intercepted numeric literal: '%s'", expanded);

    char *args[] = { "echo", (char *)expanded, NULL };
    char **cmds[] = { args };

    LOG(LOG_LEVEL_INFO, "Launching echo pipeline for literal '%s'", expanded);
    launch_pipeline(shell, cmds, 1);

    return true;
}

char **split_on_semicolons(const char *input) {
    if (!input) return NULL; // Defensive: null input yields null output

    size_t len = strlen(input);

    // Make a modifiable copy of the input string
    // We'll overwrite semicolons with '\0' to split in-place
    char *copy = strdup(input);
    if (!copy) return NULL; // Allocation failed

    // Allocate space for output segments
    // Worst case: every character is a segment (overkill, but safe)
    char **segments = calloc(len + 1, sizeof(char *));
    if (!segments) {
        free(copy);
        return NULL;
    }

    int seg_count = 0;       // Number of segments found
    char *start = copy;      // Start of current segment
    char *p = copy;          // Current scanning pointer
    char quote = '\0';       // Tracks whether we're inside quotes

    while (*p) {
        // Handle quote tracking
        if (*p == '\'' || *p == '"') {
            if (quote == '\0') {
                quote = *p; // Entering quoted region
            } else if (quote == *p) {
                quote = '\0'; // Exiting quoted region
            }
        }
        // If we hit a semicolon outside quotes, it's a split point
        else if (*p == ';' && quote == '\0') {
            *p = '\0'; // Null-terminate current segment
            segments[seg_count++] = strdup(start); // Copy segment
            start = p + 1; // Move start to next character
        }
        ++p;
    }

    // Add final segment (if any)
    if (*start) {
        segments[seg_count++] = strdup(start);
    }

    segments[seg_count] = NULL; // Null-terminate the array

    free(copy); // Free the temporary buffer
    return segments;
}