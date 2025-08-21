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


void append_to_buffer(char **buf, const char *chunk) {
    size_t oldlen = (*buf) ? strlen(*buf) : 0;
    size_t addlen = strlen(chunk);
    // +2 = one for possible newline, one for NUL
    char *newbuf = realloc(*buf, oldlen + addlen + 2);
    if (!newbuf) return; // handle OOM if you want
    if (oldlen > 0) {
        newbuf[oldlen] = '\n';
        memcpy(newbuf + oldlen + 1, chunk, addlen + 1); // +1 for NUL
    } else {
        memcpy(newbuf, chunk, addlen + 1);
    }
    *buf = newbuf;
}

void free_buffer(char **buf) {
    if (*buf) {
        free(*buf);
        *buf = NULL;
    }
}

bool is_command_complete(const char *cmd) {
    bool in_single = false, in_double = false, escaped = false;
    for (const char *p = cmd; *p; p++) {
        if (escaped) { escaped = false; continue; }
        if (*p == '\\') { escaped = true; continue; }
        if (*p == '\'' && !in_double) in_single = !in_single;
        else if (*p == '"' && !in_single) in_double = !in_double;
    }
    return !in_single && !in_double;
}




int read_input(ShellContext *ctx, bool continuation) {
    getcwd(ctx->cwd, sizeof(ctx->cwd)); // update cwd for prompt
    char prompt[512];

    if (continuation) {
        snprintf(prompt, sizeof(prompt),
                 "🔪 THRASH wants closure 🔪 ");
    } else {
        snprintf(prompt, sizeof(prompt), "\001\033[38;2;186;114;4m\002THRASH)\001\033[0m\002 \001\033[38;2;43;;214m\002%.450s\001\033[0m\002: ", ctx->cwd);
    }

    char *line = readline(prompt);
    if (!line) return 0; // Ctrl+D / EOF

    strncpy(ctx->input, line, sizeof(ctx->input) - 1);
    ctx->input[sizeof(ctx->input) - 1] = '\0';
    free(line);
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

    // Make a modifiable copy of the input string — we will insert NULs here
    char *copy = strdup(input);
    if (!copy) return NULL;

    // Allocate space for output segments (+1 for NULL terminator)
    // Worst case: every character is a delimiter, so len+1 segments
    char **segments = calloc(len + 2, sizeof(char *));
    if (!segments) {
        free(copy);
        return NULL;
    }

    int seg_count = 0;
    char *start = copy; // Marks the beginning of the current segment
    char *p = copy;     // Current scan position
    char quote = '\0';  // Quote state: '\0' means unquoted, otherwise holds quote char
    int escape = 0;     // Escape state: 1 means next char is literal

    while (*p) {
        if (escape) {
            /*
             * Previous char was a backslash, so this char is taken literally.
             * This bypasses quote toggling and delimiter checks.
             */
            escape = 0; // Reset escape after consuming one char
            ++p;
            continue;
        }

        if (*p == '\\') {
            /*
             * Found a backslash — set escape so the next char is treated literally.
             * Do not include '\' itself in any special logic.
             */
            escape = 1;
            // Optionally: memmove(p, p+1, strlen(p)); to strip '\' from output
            ++p;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            /*
             * Quote handling:
             * - If unquoted, enter quoted mode using this char as the delimiter.
             * - If already quoted with the same char, close quote.
             * Inside quotes, delimiters like ';' are ignored.
             */
            if (quote == '\0') {
                quote = *p; // Enter quoted mode
            } else if (quote == *p) {
                quote = '\0'; // Exit quoted mode
            }
            ++p;
            continue;
        }

        if (((*p == ';') || (*p == '\n')) && (quote == '\0')) {
            /*
             * We found a delimiter (semicolon or newline) while NOT in quotes
             * and NOT escaped (escape is always 0 here because that branch returns early).
             */
            if (p > start && *(p - 1) == '\r') {
                *(p - 1) = '\0'; // Trim carriage return from CRLF endings
            }
            *p = '\0'; // Terminate the current segment string

            if (*start) {
                // Only store non-empty segments
                segments[seg_count++] = strdup(start);
                LOG(LOG_LEVEL_INFO,
                    "[split] segment[%d]: '%s'\n",
                    seg_count - 1,
                    segments[seg_count - 1]);
            }

            start = p + 1; // New segment starts after the delimiter
            ++p;
            continue;
        }

        // Default: just advance to next character
        ++p;
    }

    // After loop ends, handle the final segment (if non-empty)
    if (*start) {
        segments[seg_count++] = strdup(start);
    }

    segments[seg_count] = NULL; // NULL-terminate the array

    free(copy);
    return segments;
}