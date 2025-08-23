// signals.h
#ifndef SIGNALS_H
#define SIGNALS_H

#include "shell.h"

// Called in the child (just before execve)
void setup_child_signals(void);

// (Optional) Called in the parent after fork
void setup_parent_signals(void);

void reclaim_terminal(ShellContext *shell);

void give_terminal_to_pgid(ShellContext *shell, pid_t pgid);

#endif // SIGNALS_H