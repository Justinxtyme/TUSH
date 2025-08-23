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


/*int perform_redirections(Command *cmd) {
    if (!cmd) return 0;

    LOG(LOG_LEVEL_INFO, "redir: begin for argv[0]=%s",
        (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "(null)");

    // CWD override
    if (cmd->cwd_override) {
        if (chdir(cmd->cwd_override) != 0) {
            LOG(LOG_LEVEL_ERR, "redir: chdir('%s') failed: %s", cmd->cwd_override, strerror(errno));
            return -1;
        }
        LOG(LOG_LEVEL_INFO, "redir: cwd overridden to '%s'", cmd->cwd_override);
    }

    // Heredoc overrides input_file
    if (cmd->heredoc && cmd->input_file) {
        LOG(LOG_LEVEL_WARN, "redir: both heredoc and input_file set—heredoc will override stdin");
    }

    // Input redirection: heredoc or input_file
    if (cmd->heredoc) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            LOG(LOG_LEVEL_ERR, "pipe failed for heredoc: %s", strerror(errno));
            return -1;
        }

        ssize_t written = write(pipefd[1], cmd->heredoc, strlen(cmd->heredoc));
        if (written < 0) {
            LOG(LOG_LEVEL_ERR, "write failed for heredoc: %s", strerror(errno));
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }

        close(pipefd[1]);

        if (dup2(pipefd[0], STDIN_FILENO) < 0) {
            LOG(LOG_LEVEL_ERR, "dup2 failed for heredoc pipe: %s", strerror(errno));
            close(pipefd[0]);
            return -1;
        }

        close(pipefd[0]);
        LOG(LOG_LEVEL_INFO, "heredoc injected into stdin");
    } else if (cmd->input_file) {
        int target_fd = (cmd->input_fd >= 0) ? cmd->input_fd : STDIN_FILENO;
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for input (fd=%d)", cmd->input_file, target_fd);

        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s", cmd->input_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, target_fd) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s", fd, target_fd, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: input now from '%s'", cmd->input_file);
    }

    // Output redirection: append or truncate
    const char *out_target = cmd->append_file ? cmd->append_file : cmd->output_file;
    if (out_target) {
        int target_fd = (cmd->output_fd >= 0) ? cmd->output_fd : STDOUT_FILENO;
        int flags = O_WRONLY | O_CREAT | (cmd->append_file ? O_APPEND : O_TRUNC);
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for output (fd=%d)", out_target, target_fd);

        int fd = open(out_target, flags, 0666);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s", out_target, strerror(errno));
            return -1;
        }
        if (dup2(fd, target_fd) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s", fd, target_fd, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: output now to '%s'", out_target);
    }

    // Error redirection
    if (cmd->error_file) {
        int target_fd = (cmd->error_fd >= 0) ? cmd->error_fd : STDERR_FILENO;
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for error output (fd=%d)", cmd->error_file, target_fd);

        int fd = open(cmd->error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s", cmd->error_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, target_fd) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s", fd, target_fd, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: error output now to '%s'", cmd->error_file);
    }

    // Combined redirection
    if (cmd->output_to_error) {
        if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(stdout->stderr) failed: %s", strerror(errno));
            return -1;
        }
        LOG(LOG_LEVEL_INFO, "redir: stderr now mirrors stdout");
    }
    if (cmd->error_to_output) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(stderr->stdout) failed: %s", strerror(errno));
            return -1;
        }
        LOG(LOG_LEVEL_INFO, "redir: stdout now mirrors stderr");
    }
    LOG(LOG_LEVEL_INFO, "redir: done");
    return 0;
} */



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




