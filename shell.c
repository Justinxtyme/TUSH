#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // open, dup, getpid, setpgid, tcsetpgrp
#include <fcntl.h>      // O_RDWR, O_CLOEXEC
#include <errno.h>      // errno, EACCES
#include <signal.h>

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

// job control
void setup_shell_job_control(ShellContext *shell) {
    // Open controlling terminal
    shell->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (shell->tty_fd < 0) {
        // Fallback to stdin if /dev/tty not available
        shell->tty_fd = dup(STDIN_FILENO);
    }

    // Put shell in its own process group
    shell->shell_pgid = getpid();
    if (setpgid(0, shell->shell_pgid) < 0 && errno != EACCES) {
        perror("setpgid(shell)");
    }

    // Make shell the foreground job on the terminal
    if (tcsetpgrp(shell->tty_fd, shell->shell_pgid) < 0) {
        // If this stops us (SIGTTOU) in a weird state, we’ll prevent it below by ignoring SIGTTOU.
    }

    // Shell should ignore job-control signals
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;

    sigaction(SIGTSTP, &sa, NULL); // do not allow shell itself to be stopped
    sigaction(SIGTTIN, &sa, NULL); // avoid stop on tty reads while bg
    sigaction(SIGTTOU, &sa, NULL); // avoid stop on tty writes/tcsetpgrp while bg

    // Also typically ignore SIGINT and SIGQUIT in the shell itself
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}