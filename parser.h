#ifndef PARSER_H
#define PARSER_H

#include "command.h"

Command **parse_commands(const char *input, int *num_cmds);

#endif