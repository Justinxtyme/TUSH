// signals.h
#ifndef SIGNALS_H
#define SIGNALS_H

// Called in the child (just before execve)
void setup_child_signals(void);

// (Optional) Called in the parent after fork
void setup_parent_signals(void);

#endif // SIGNALS_H