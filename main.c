/* this is an attempt to create a simple shell in C, called "TUSH", Totally Unnecessary Shell, Totally Useless Shell, or The Ultimate Shell.
 it will support basic built-in commands like 'cd', 'exit', and 'ls'   
 and other posix commands */


#include "shell.h" // Include the shell context and function declarations
#include "executor.h" // Include the command execution function
#include <stdio.h> // for printf, fgets, perror
#include <stdlib.h> // for exit,
#include <string.h> // for str maniopulation functions
#ifdef _WIN32
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h> //// for POSIX functions like fork, execvp, chdir
#endif 

// --- Prompt Display ---
void display_prompt(ShellContext *ctx) { //// Display the shell prompt
    getcwd(ctx->cwd, sizeof(ctx->cwd)); // Get current working directory
    printf("TUSH %s: ", ctx->cwd); // Display prompt with current directory
    //printf("TUSH> "); // Prompt for user input
    fflush(stdout); // Ensure the prompt is printed immediately
}

// --- Input Handling ---
int read_input(ShellContext *ctx) { // Read input from stdin
    if (!fgets(ctx->input, sizeof(ctx->input), stdin)) { // Read a line of input
        return 0; // EOF or error
    }
    ctx->input[strcspn(ctx->input, "\n")] = 0; // Strip newline
    return 1;
}

// --- Tokenizer (temporary inline version) ---
char **tokenize_input(char *input) {
    static char *args[64]; // max 64 tokens
    int i = 0;
    char *token = strtok(input, " ");
    while (token != NULL && i < 63) {
        args[i++] = token;
        token = strtok(NULL, " ");
    }
    args[i] = NULL;
    return args;
}


// --- Main Loop ---
int main() {
    ShellContext shell = { .running = 1 }; // Initialize shell context with running flag set to 1
    init_shell(&shell); // Initialize the shell context
    
    while (shell.running) {
        display_prompt(&shell); // Display the shell prompt

        if (!read_input(&shell)) {  
            perror("fgets failed");
            break; // Ctrl+D or error
        }

       add_to_history(&shell, shell.input); // adds input to history, for reuse

        char **args = tokenize_input(shell.input);
        if (args[0] == NULL) continue; // empty input

        if (strcmp(args[0], "exit") == 0) {
            shell.running = 0;
        } else {
            run_command(args); // ⬅️ Dispatch to executor
        }
    }

    return 0;
}





 /*void display_prompt() {
    printf("mytush> "); // Display the shell prompt
    fflush(stdout); // Ensure prompt is printed immediately
}

int main() {
    char input[1024]; // Buffer to hold user input

    while (1) {
        display_prompt(); // Show prompt

        if (!fgets(input, sizeof(input), stdin)) { // Read input from stdin
            perror("fgets failed");
            break; // Exit on EOF (Ctrl+D)
        }

        // Strip newline
        input[strcspn(input, "\n")] = 0; // Remove trailing newline character

        // Temporary: echo back the input
        printf("You entered: %s\n", input); // Echo the input for debugging

        // Exit command
        if (strcmp(input, "exit") == 0) { // Check for exit command
            break;
        }
    }

    return 0;
}  */

