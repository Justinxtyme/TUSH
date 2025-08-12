// signals.c
#include "signals.h"
#include <signal.h>
#include <termios.h>    // tcsetpgrp()
#include <unistd.h>     // fork(), pipe(), dup2(), etc.
#include <sys/wait.h>   // waitpid(), WIFEXITED, etc.
#include <string.h>     // strcmp()
#include <stdio.h>      // perror()




/* Called once in main() before REPL to ignore SIGINT/SIGQUIT in the shell */
void setup_parent_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

/* Called in every child before exec to restore default signal behavior */
void setup_child_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}
