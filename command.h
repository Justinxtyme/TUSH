#ifndef COMMAND_H
#define COMMAND_H

#include <termios.h>    // tcsetpgrp()
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>


typedef struct {
    char **argv;               // Command and arguments
    int argc;                  // tracking count 
    char *input_file;          // For `<`
    char *output_file;         // For `>`
    char *append_file;         // For `>>`
    char *error_file;          // For `2>`
    bool output_to_error;      // For `2>&1`
    bool error_to_output;      // For `&>` or `>&`
    int input_fd;              // For `n<file` (e.g., `3<foo`)
    int output_fd;             // For `n>file` (e.g., `4>bar`)
    int error_fd;              // For `n>&m` (e.g., `2>&1`)
    bool background;           // For trailing `&`
    bool is_builtin;           // Flag for built-in command
    pid_t pgid;                // Process group ID (for job control)
    char *heredoc;             // For `<<EOF` style input
    char *cwd_override;        // For `cd` or directory-specific exec
    char *raw_input;           // Original input string (for debugging/logging)
} Command;

void free_command(Command *cmd);

void free_command_list(Command **cmds, int num_cmds);

#endif