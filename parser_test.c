#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h> // for pid_t
#include <sys/types.h>

#define MAX_CMDS 10
#define MAX_ARGS 20

typedef struct {
    char **argv;
    int argc;
    char *input_file;
    char *output_file;
    char *append_file;
    char *error_file;
    bool is_append;
    bool output_to_error;
    bool error_to_output;
    int input_fd;
    int output_fd;
    int error_fd;
    bool background;
    bool is_builtin;
    pid_t pgid;
    char *heredoc;
    char *cwd_override;
    char *raw_input;
} Command;

// === Your original parse_pipeline() goes here ===
// Paste it exactly as-is, no changes.
// I’ll skip repeating it here since you already posted it.
Command **parse_pipeline(const char *input, int *num_cmds) {
    Command **cmds = calloc(MAX_CMDS, sizeof(Command *));
    if (!cmds) return NULL;

    Command *cmd = calloc(1, sizeof(Command));
    if (!cmd) return NULL;

    cmd->argv = calloc(MAX_ARGS, sizeof(char *));
    if (!cmd->argv) return NULL;

    int cmd_index = 0;
    int arg_index = 0;
    char token_buff[1024];
    int buff_index = 0;

    const char *p = input;
    bool in_single = false, in_double = false;
    bool in_redirect = false, is_append = false;
    int redir_fd = -1;

    while (*p) {
        char c = *p;

        if (c == '\\' && p[1]) {
            token_buff[buff_index++] = p[1];
            p += 2;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
            p++;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
            p++;
        } else if (c == '|' && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                cmd->argv[arg_index++] = strdup(token_buff);
                buff_index = 0;
            }

            cmd->argv[arg_index] = NULL;
            cmd->argc = arg_index;
            cmds[cmd_index++] = cmd;

            cmd = calloc(1, sizeof(Command));
            cmd->argv = calloc(MAX_ARGS, sizeof(char *));
            arg_index = 0;
            p++;
        } else if (isspace(c) && !in_single && !in_double) {
            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                cmd->argv[arg_index++] = strdup(token_buff);
                buff_index = 0;
            }
            p++;
        } else if ((c == '>' || c == '<') && !in_single && !in_double && !in_redirect) {
            char chevron = c;
            in_redirect = true;
            redir_fd = -1;

            if (buff_index > 0) {
                token_buff[buff_index] = '\0';
                cmd->argv[arg_index++] = strdup(token_buff);
                buff_index = 0;
            }

            if (*(p + 1) == c) {
                is_append = true;
                p += 2;
            } else if (isdigit(*(p - 1))) {
                redir_fd = *(p - 1) - '0';
                p++;
            } else {
                p++;
            }

            while (*p) {
                c = *p;
                if (isspace(c) && !in_single && !in_double) {
                    if (buff_index > 0) {
                        token_buff[buff_index] = '\0';

                        if (chevron == '>') {
                            cmd->output_file = strdup(token_buff);
                            cmd->is_append = is_append;
                            cmd->output_fd = (redir_fd != -1) ? redir_fd : 1;
                        } else if (chevron == '<') {
                            cmd->input_file = strdup(token_buff);
                            cmd->input_fd = (redir_fd != -1) ? redir_fd : 0;
                        }

                        buff_index = 0;
                        is_append = false;
                        in_redirect = false;
                    }
                    p++;
                    break;
                } else if (c == '\'' && !in_double) {
                    in_single = !in_single;
                    p++;
                } else if (c == '"' && !in_single) {
                    in_double = !in_double;
                    p++;
                } else {
                    token_buff[buff_index++] = c;
                    p++;
                }
            }
        } else if (strncmp(p, "2>&1", 4) == 0 && !in_single && !in_double) {
            cmd->output_to_error = true; // NEW: track 2>&1
            p += 4;
        } else {
            token_buff[buff_index++] = c;
            p++;
        }
    }

    if (buff_index > 0) {
        token_buff[buff_index] = '\0';
        cmd->argv[arg_index++] = strdup(token_buff);
    }

    cmd->argv[arg_index] = NULL;
    cmd->argc = arg_index;

    // Check for trailing '&' in argv
    if (arg_index > 0 && strcmp(cmd->argv[arg_index - 1], "&") == 0) {
        cmd->background = true;
        free(cmd->argv[--arg_index]); // remove '&' from argv
        cmd->argv[arg_index] = NULL;
        cmd->argc = arg_index;
    }

    cmds[cmd_index++] = cmd;
    *num_cmds = cmd_index;
    return cmds;
}

void print_command(Command *cmd, int index) {
    printf("Command %d:\n", index);
    printf("  argc: %d\n", cmd->argc);
    printf("  argv:");
    for (int i = 0; i < cmd->argc; i++) {
        printf(" \"%s\"", cmd->argv[i]);
    }
    printf("\n");

    if (cmd->input_file)
        printf("  input_file: %s (fd: %d)\n", cmd->input_file, cmd->input_fd);
    if (cmd->output_file)
        printf("  output_file: %s (fd: %d)\n", cmd->output_file, cmd->output_fd);
    if (cmd->append_file)
        printf("  append_file: %s\n", cmd->append_file);
    if (cmd->error_file)
        printf("  error_file: %s (fd: %d)\n", cmd->error_file, cmd->error_fd);

    if (cmd->output_to_error)
        printf("  output_to_error: true\n");
    if (cmd->error_to_output)
        printf("  error_to_output: true\n");

    if (cmd->background)
        printf("  background: true\n");
    if (cmd->is_builtin)
        printf("  is_builtin: true\n");

    if (cmd->heredoc)
        printf("  heredoc: %s\n", cmd->heredoc);
    if (cmd->cwd_override)
        printf("  cwd_override: %s\n", cmd->cwd_override);
    if (cmd->raw_input)
        printf("  raw_input: %s\n", cmd->raw_input);

    printf("\n");
}

void free_command(Command *cmd) {
    if (!cmd) return;
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    free(cmd->input_file);
    free(cmd->output_file);
    free(cmd->append_file);
    free(cmd->error_file);
    free(cmd->heredoc);
    free(cmd->cwd_override);
    free(cmd->raw_input);
    free(cmd);
}

int main() {
    const char *test_inputs[] = {
        "echo hello >out.txt",
        "grep foo <input.txt >>append.log",
        "ls -l | grep '^d' >dirs.txt",
        "cat file.txt | sort | uniq -c >>counts.log",
        "echo 'hi there' >out.txt 2>&1",
        "cmd1 | cmd2 &",
        "cd thrash; ls -l | grep 'yellow' >out.txt",
        NULL
    };

    for (int i = 0; test_inputs[i]; i++) {
        printf("=== Test %d: \"%s\" ===\n", i + 1, test_inputs[i]);
        int num_cmds = 0;

        // Use your original parser
        Command **cmds = (Command **)parse_pipeline(test_inputs[i], &num_cmds);
        if (!cmds) {
            fprintf(stderr, "Parsing failed\n");
            continue;
        }

        for (int j = 0; j < num_cmds; j++) {
            print_command(cmds[j], j);
            free_command(cmds[j]); // frees argv, input/output files, etc.
        }
        free(cmds); // NEW: free the heap-allocated array of Command pointers
    }

    return 0;
}

