// input.c 
/* This file implements input handling for the TUSH shell
 It uses the readline library for enhanced user input features */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h> 
#include <readline/history.h> 
#include "input.h" // 
#include <unistd.h> // for getcwd

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
    getcwd(ctx->cwd, sizeof(ctx->cwd) - 8); // update cwd for prompt
    char prompt[512]; //
    snprintf(prompt, sizeof(prompt), "TUSH %.502s: ", ctx->cwd); // %.502s limits to 502 chars to avoid overflow

    char *line = readline(prompt);
    if (!line) return 0; // Ctrl+D or EOF

    if (*line) add_history(line); // non-empty input gets saved

    strncpy(ctx->input, line, sizeof(ctx->input) - 1);
    ctx->input[sizeof(ctx->input) - 1] = '\0'; // ensure null-termination

    free(line); // readline allocates with malloc
    return 1;
}