// signals.c
#include "signals.h"
#include <signal.h>
#include <termios.h>    // tcsetpgrp()
#include <unistd.h>     // fork(), pipe(), dup2(), etc.
#include <sys/wait.h>   // waitpid(), WIFEXITED, etc.
#include <string.h>     // strcmp()
#include <stdio.h>      // perror()
#include <errno.h>
#include "debug.h"
#include "shell.h"




/* In main(), call once before the REPL so the shell ignores these signals itself */
void setup_parent_signals(void) {
    // Initializes a sigaction structure, define how a process responds to a specific signal.
    // Setting it to {0} clears all its members.
    struct sigaction sa = {0}; 
    //Sets the sa_handler member of the sigaction structure to SIG_IGN. 
    //SIG_IGN indicates that the specified signals should be ignored by the process.
    sa.sa_handler = SIG_IGN; //SIG ignore
    sigaction(SIGINT,  &sa, NULL); // ctrl C
    sigaction(SIGQUIT, &sa, NULL); // ctrl /
    sigaction(SIGTSTP, &sa, NULL); // ctrl z
}

/* In each child, restore default signal actions */
void setup_child_signals(void) {
    struct sigaction sa = {0};
    //SIG_DFL indicates that the specified signals should revert to their default actions
    sa.sa_handler = SIG_DFL; // SIG default
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL); //terminal input sig
    sigaction(SIGTTOU, &sa, NULL); //terminal output sig
    sigaction(SIGCHLD, &sa, NULL);
}

void give_terminal_to_pgid(ShellContext *shell, pid_t pgid) { 
    // With SIGTTOU ignored, tcsetpgrp won’t stop us if we happen to be bg.
    if (tcsetpgrp(shell->tty_fd, pgid) < 0) { 
        // Don’t spam; log if you have a debug flag
        LOG(LOG_LEVEL_INFO, "tcsetpgrp(give): %s\n", strerror(errno));

    }
}

void reclaim_terminal(ShellContext *shell) {
    if (tcsetpgrp(shell->tty_fd, shell->shell_pgid) < 0) {
        // perror("tcsetpgrp(reclaim)");
    }
}