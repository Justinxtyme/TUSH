/*shell.h is the header file for the TUSH shell
 It defines the ShellContext structure and function prototypes for shell operations
 It includes necessary libraries and defines constants for input size and history management
 It is used to manage the shell's state, user input, and command history */

#include <sys/types.h>
 #ifndef SHELL_H
#define SHELL_H

#define INPUT_SIZE 1024

typedef struct {
    char input[INPUT_SIZE];   // User input buffer
    int running;              // Shell loop control flag
    int  last_status;
    pid_t last_pgid;    // new field
    char cwd[512];            // Current working directory
    char **history;         // Array of command strings
    int history_size;       // Number of stored commands
    int history_capacity;   // Allocated slots    
    // Add more fields as needed: history, env vars, etc.
} ShellContext;

void init_shell(ShellContext *ctx); // Initialize shell context

void add_to_history(ShellContext *ctx, const char *input); // Add command to history

#endif

