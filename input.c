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

/*BASIC ANSI CODES FOR COLOR
UNCOMMENT IF USING
#define COLOR_THRASH  "\x1b[32m" // green
#define STYLE_RESET "\x1b[0m" 
#define COLOR_CTX "\x1b[35m" // magenta
#define BOLD "\x1b[1m" // bold/bright
#define REVERSE "\x1b[4m"*/


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
    
   /* const char *sh_color = COLOR_THRASH;
    //const char *ctx_color = COLOR_CTX;
    //snprintf(prompt, sizeof(prompt), "%s%s%sTHRASH%s) %s%s%.450s%s: ", //ORIGINAL
     //sh_color, BOLD, REVERSE, STYLE_RESET, ctx_color, BOLD, ctx->cwd, STYLE_RESET); // %.502s limits to 502 chars to avoid overflow //ORIGINAL

    //snprintf(prompt, sizeof(prompt), "\001%s%s%s\002THRASH\001%s\002) \001%s%s\002%.450s\001%s\002: ",*/
    snprintf(prompt, sizeof(prompt), "\001\033[38;2;186;114;4m\002THRASH)\001\033[0m\002 \001\033[38;2;43;;214m\002%.450s\001\033[0m\002: ",
    ctx->cwd);

    char *line = readline(prompt);
    if (!line) return 0; // Ctrl+D or EOF

    //if (*line) add_history(line); // non-empty input gets saved
    strncpy(ctx->input, line, sizeof(ctx->input) - 1); // 
    ctx->input[sizeof(ctx->input) - 1] = '\0'; // ensure null-termination

    free(line); // readline allocates with malloc
    return 1;
}

bool is_numeric(const char *s) {
    if (!s) return false;

    // Skip leading whitespace
    while (isspace((unsigned char)*s)) s++;

    if (!*s) return false; // if false, input empty after trimming

    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }

    return true;
}

bool handle_literal_expansion(ShellContext *shell, Command *cmd) {
    if (!is_numeric(cmd->argv[0])) {
        return false;
    }

    LOG(LOG_LEVEL_INFO, "Intercepted numeric literal: '%s'", cmd->argv[0]);

    // Optional: rewrite argv to echo the literal
    char *args[] = { "echo", cmd->argv[0], NULL };
    cmd->argv = args;
    cmd->argc = 2;
    cmd->is_builtin = true;  // if echo is handled as a builtin

    Command *cmds[] = { cmd };
    launch_pipeline(shell, cmds, 1);

    return true;
}



// Note: whitespace trimming deferred to parse_pipeline()
// This function only handles quote-aware semicolon splitting
char **split_on_semicolons(const char *input) {
    if (!input) return NULL; // Defensive: null input yields null output

    size_t len = strlen(input);
    if (len == 0) return NULL; // Empty input yields null output

    // Make a modifiable copy of the input string
    char *copy = strdup(input);
    if (!copy) return NULL;

    // Allocate space for output segments (+1 for NULL terminator)
    char **segments = calloc(len + 2, sizeof(char *));
    if (!segments) {
        free(copy);
        return NULL;
    }

    int seg_count = 0;
    char *start = copy;
    char *p = copy;
    char quote = '\0';

    while (*p) {
        if (*p == '\'' || *p == '"') {
            if (quote == '\0') {
                quote = *p;
            } else if (quote == *p) {
                quote = '\0';
            }
        } else if ((*p == ';') || (*p == '\n') && (quote == '\0')) {
            *p = '\0';
            if (*start) {
                segments[seg_count++] = strdup(start);
            }
            start = p + 1;
        }
        ++p;
    }

    // Final segment
    if (*start) {
        segments[seg_count++] = strdup(start);
    }

    segments[seg_count] = NULL; // NULL-terminate the array
    free(copy);
    return segments;
}
