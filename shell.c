#include <stdio.h>
#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // open, dup, getpid, setpgid, tcsetpgrp
#include <fcntl.h>      // O_RDWR, O_CLOEXEC
#include <errno.h>      // errno, EACCES
#include <signal.h>


// job control
void setup_shell_job_control(ShellContext *shell) {  
    // Open controlling terminal
    shell->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC); // Open the controlling terminal
    if (shell->tty_fd < 0) {
        // Fallback to stdin if /dev/tty not available
        shell->tty_fd = dup(STDIN_FILENO); // Duplicate stdin
    }

    // Put shell in its own process group
    shell->shell_pgid = getpid(); // Get the shell's process group ID
    if (setpgid(0, shell->shell_pgid) < 0 && errno != EACCES) { // EACCES means we are already in a group
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