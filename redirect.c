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

int perform_redirections(Command *cmd) {
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
}







