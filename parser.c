#include <stdio.h>
#include <string.h>
#include <stdlib.h> // Required for malloc and free
#include <ctype.h>
#include "command.h"
#include "debug.h"

#define MAX_CMDS 16
#define MAX_ARGS 64

Command **parse_commands(const char *input, int *num_cmds) {
    Command **cmds = calloc(MAX_CMDS, sizeof(Command *));
    if (!cmds) {
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }

    int cmd_index = 0;
    int arg_index = 0;
    int buff_index = 0;

    char token_buff[1024];
    bool in_single = false, in_double = false;
    const char *p = input;
    bool aborted = false;

    // Allocate first Command
    Command *current = calloc(1, sizeof(Command));
    if (!current) {
        free(cmds);
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }
    current->argv = calloc(MAX_ARGS, sizeof(char *));
    if (!current->argv) {
        free(current);
        free(cmds);
        if (num_cmds) *num_cmds = 0;
        return NULL;
    }

    while (*p) {
        char c = *p;

        // 1) Escape sequence: copy next char verbatim
        if (c == '\\' && p[1]) {
            if (buff_index < (int)sizeof(token_buff) - 1) {
                token_buff[buff_index++] = p[1];
            }
            p += 2;
            continue;
        }

        // 2) Quote toggles
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            p++;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            p++;
            continue;
        }

        // 3) Pipe: end current command, start next
        if (c == '|' && !in_single && !in_double) {
            // flush any pending word
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                if (arg_index < MAX_ARGS - 1) {
                    current->argv[arg_index++] = strdup(token_buff);
                }
                buff_index = 0;
            }

            // terminate current->argv
            current->argv[arg_index] = NULL;
            current->argc = arg_index;

            // store command
            if (cmd_index < MAX_CMDS) {
                cmds[cmd_index++] = current;
            } else {
                LOG(LOG_LEVEL_ERR, "Too many commands, discarding extra");
                free_command(current);
                aborted = true;
                break;
            }

            // allocate next Command
            current = calloc(1, sizeof(Command));
            if (!current) { aborted = true; break; }
            current->argv = calloc(MAX_ARGS, sizeof(char *));
            if (!current->argv) { free(current); aborted = true; break; }

            arg_index = 0;
            p++;
            continue;
        }

        // 4) Redirection: >, >>, <
        if ((c == '>' || c == '<') && !in_single && !in_double) {
            char chevron = c;
            bool is_append = (c == '>' && p[1] == '>');
            int specified_fd = -1;

            // If the token buffer is all digits, consume it as a FD spec
            if (buff_index > 0) {
                bool all_digits = true;
                for (int i = 0; i < buff_index; i++) {
                    if (token_buff[i] < '0' || token_buff[i] > '9') {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    specified_fd = atoi(token_buff);
                    buff_index = 0;
                } else {
                    // flush it as a real argv word
                    token_buff[buff_index] = '\0';
                    if (arg_index < MAX_ARGS - 1) {
                        current->argv[arg_index++] = strdup(token_buff);
                    }
                    buff_index = 0;
                }
            }

            // skip the chevron(s)
            p += is_append ? 2 : 1;

            // skip spaces before filename
            while (*p && isspace((unsigned char)*p)) p++;

            // parse the filename (respecting quotes)
            buff_index = 0;
            while (*p && (!isspace((unsigned char)*p) || in_single || in_double)) {
                if (*p == '\'' && !in_double) {
                    in_single = !in_single;
                    p++;
                } else if (*p == '"' && !in_single) {
                    in_double = !in_double;
                    p++;
                } else {
                    if (buff_index < (int)sizeof(token_buff) - 1) {
                        token_buff[buff_index++] = *p;
                    }
                    p++;
                }
            }
            token_buff[buff_index] = '\0';
            char *filename = strdup(token_buff);
            buff_index = 0;

            // assign to input/output as appropriate
            if (chevron == '<') {
                current->input_file = filename;
                current->input_fd = (specified_fd != -1) ? specified_fd : 0;
            } else if (chevron == '>' && is_append) {
                current->append_file = filename;
                current->output_fd = (specified_fd != -1) ? specified_fd : 1;
            } else {
                current->output_file = filename;
                current->output_fd = (specified_fd != -1) ? specified_fd : 1;
            }

            // do not push filename to argv
            continue;
        }

        // 5) Whitespace: flush token
        if (isspace((unsigned char)c) && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                if (arg_index < MAX_ARGS - 1) {
                    current->argv[arg_index++] = strdup(token_buff);
                }
                buff_index = 0;
            }
            p++;
            continue;
        }

        // 6) Regular character: accumulate
        if (buff_index < (int)sizeof(token_buff) - 1) {
            token_buff[buff_index++] = c;
        }
        p++;
    }

    // Final flush if not aborted
    if (!aborted) {
        if (buff_index > 0) {
            token_buff[buff_index] = '\0';
            if (arg_index < MAX_ARGS - 1) {
                current->argv[arg_index++] = strdup(token_buff);
            }
        }
        current->argv[arg_index] = NULL;
        current->argc = arg_index;

        if (cmd_index < MAX_CMDS) {
            cmds[cmd_index++] = current;
        } else {
            LOG(LOG_LEVEL_ERR, "Final command exceeds MAX_CMDS, discarding");
            free_command(current);
        }
    }

    if (num_cmds) *num_cmds = cmd_index;
    return cmds;
}