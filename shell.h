/*shell.h is the header file for the THRASH shell
 It defines the ShellContext structure and function prototypes for shell operations
 It includes necessary libraries and defines constants for input size and history management
 It is used to manage the shell's state, user input, and command history */

#include <sys/types.h>
#include "history.h"
#include "var.h" // Include variable table definitions
 #ifndef SHELL_H
#define SHELL_H

#define INPUT_SIZE 1024

typedef struct {
    char input[INPUT_SIZE];   // User input buffer
    int running;              // Shell loop control flag
    int  last_status; // Last command exit status
    int   tty_fd; // Terminal file descriptor
    pid_t shell_pgid; // Shell process group ID
    pid_t last_pgid;    // Last foreground process group ID
    pid_t pipeline_pgid; // Current pipeline process group ID
    char cwd[512];            // Current working directory
    History history; // Command history
    VarTable *vars; // Hash table for variables
} ShellContext;

void add_to_history(ShellContext *ctx, const char *input); // Add command to history

void setup_shell_job_control(ShellContext *shell);
#endif

