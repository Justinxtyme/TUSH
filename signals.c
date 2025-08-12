// signals.c
#include "signals.h"
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>




/* Called once in main() before REPL to ignore SIGINT/SIGQUIT in the shell */
static void setup_parent_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

/* Called in every child before exec to restore default signal behavior */
static void setup_child_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}
