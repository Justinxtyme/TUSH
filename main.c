/* this is an attempt to create a simple shell in C, called "TUSH", Totally Unnecessary Shell, Totally Useless Shell, or The Ultimate Shell.
 it will support basic built-in commands like 'cd', 'exit', and 'ls'   
 and other posix commands */

#include "input.h"
#include "shell.h" // Include the shell context and function declarations
#include "executor.h" // Include the command execution function
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdio.h> // for printf, fgets, perror
#include <stdlib.h> // for exit,
#include <string.h> // for str maniopulation functions
#include "debug.h"


#ifdef _WIN32
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h> //// for POSIX functions like fork, execvp, chdir
#endif 



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
    initialize_readline();
   
    // Log shell startup 
    LOG(LOG_LEVEL_INFO, "TUSH started, pid=%d", getpid());
   
    while (shell.running) {
        //display_prompt(&shell); // Display the shell prompt

        if (!read_input(&shell)) {  
            LOG(LOG_LEVEL_ERR, "read_input failed: %s", strerror(errno));
            perror("readline failed");
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
    cleanup_readline();
    return 0;
}


