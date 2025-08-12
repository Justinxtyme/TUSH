// signals.c
#include "signals.h"
#include <signal.h>
#include <termios.h>    // tcsetpgrp()
#include <unistd.h>     // fork(), pipe(), dup2(), etc.
#include <sys/wait.h>   // waitpid(), WIFEXITED, etc.
#include <string.h>     // strcmp()
#include <stdio.h>      // perror()




/* In main(), call once before the REPL so the shell ignores these signals itself */
void setup_parent_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
}

/* In each child, restore default signal actions */
void setup_child_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
}