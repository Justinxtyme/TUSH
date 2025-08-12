// signals.c
#include "signals.h"
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

void setup_child_signals(void) {
    // Restore default handlers so Ctrl-C in a child kills the child, not the shell
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

void setup_parent_signals(void) {
    // Ignore SIGINT in the parent so Ctrl-C won’t kill your shell
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
}