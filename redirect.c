// redirect.c

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "redirect.h"
#include "debug.h"
#include "command.h"
#include <errno.h>

//-----------------------------------------------------------------------------
// Helper: remove two entries (redir token + filename) from cmd->argv at `pos`
//-----------------------------------------------------------------------------
/*static void remove_redir_tokens(Command *cmd, int pos) {
    // free the redirection operator and its filename
    free(cmd->argv[pos]);
    free(cmd->argv[pos + 1]);

    // shift the remaining argv entries left by two
    int read_i  = pos + 2;
    int write_i = pos;
    while (cmd->argv[read_i]) {
        cmd->argv[write_i++] = cmd->argv[read_i++];
    }

    // null-terminate and adjust argc
    cmd->argv[write_i] = NULL;
    cmd->argc        -= 2;
}

*/

//-----------------------------------------------------------------------------
// Perform all redirections in cmd, strip tokens from argv as we go.
// Returns 0 on success, -1 on failure (errno set by open()).
//-----------------------------------------------------------------------------

int extract_redirections(const Command *cmd, Redirection **out) {
    Redirection *list = calloc(8, sizeof(Redirection)); // max 8 redirs?
    int count = 0;

    if (cmd->input_file) {
        list[count++] = (Redirection){REDIR_IN, 0, -1, cmd->input_file};
    }
    if (cmd->output_file) {
        list[count++] = (Redirection){REDIR_OUT, 1, -1, cmd->output_file};
    }
    if (cmd->append_file) {
        list[count++] = (Redirection){REDIR_APPEND, 1, -1, cmd->append_file};
    }
    if (cmd->error_file) {
        list[count++] = (Redirection){REDIR_ERR, 2, -1, cmd->error_file};
    }
    if (cmd->output_to_error) {
        list[count++] = (Redirection){REDIR_DUP_OUT, 2, 1, NULL};
    }
    if (cmd->error_to_output) {
        list[count++] = (Redirection){REDIR_DUP_ERR, 1, 2, NULL};
    }
    if (cmd->heredoc) {
        list[count++] = (Redirection){REDIR_HEREDOC, 0, -1, NULL, cmd->heredoc};
    }
    if (cmd->cwd_override) {
        list[count++] = (Redirection){REDIR_CWD, -1, -1, cmd->cwd_override};
    }

    *out = list;
    return count;
}


int perform_redirections(Redirection *list, int count) {
    for (int i = 0; i < count; ++i) {
        Redirection *r = &list[i];
        switch (r->type) {
            case REDIR_IN: {
                int fd = open(r->filename, O_RDONLY);
                if (fd < 0) return -1;
                dup2(fd, r->target_fd); close(fd);
                break;
            }
            case REDIR_OUT: {
                int fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) return -1;
                dup2(fd, r->target_fd); close(fd);
                break;
            }
            case REDIR_APPEND: {
                int fd = open(r->filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (fd < 0) return -1;
                dup2(fd, r->target_fd); close(fd);
                break;
            }
            case REDIR_ERR: {
                int fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) return -1;
                dup2(fd, r->target_fd); close(fd);
                break;
            }
            case REDIR_DUP_OUT:
            case REDIR_DUP_ERR: {
                dup2(r->source_fd, r->target_fd);
                break;
            }
            case REDIR_HEREDOC: {
                int pipefd[2];
                pipe(pipefd);
                write(pipefd[1], r->heredoc_data, strlen(r->heredoc_data));
                close(pipefd[1]);
                dup2(pipefd[0], r->target_fd);
                close(pipefd[0]);
                break;
            }
            case REDIR_CWD: {
                chdir(r->filename);
                break;
            }
        }
    }
    return 0;
}




