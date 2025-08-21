// redirect.c

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "redirect.h"
#include "debug.h"
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

    // stdin: < file  (default fd 0 or cmd->input_fd)
    if (cmd->input_file) {
        int in_fd_spec = (cmd->input_fd > -1) ? cmd->input_fd : 0;
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for stdin (fd=%d)",
            cmd->input_file, in_fd_spec);

        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s",
                cmd->input_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, in_fd_spec) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s",
                fd, in_fd_spec, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: stdin now from '%s'", cmd->input_file);
    }

    // stdout: > file (truncate) OR >> file (append)
    // Prefer append_file if set; otherwise output_file.
    if (cmd->append_file) {
        int out_fd_spec = (cmd->output_fd > -1) ? cmd->output_fd : 1;
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for append (fd=%d)",
            cmd->append_file, out_fd_spec);

        int fd = open(cmd->append_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s",
                cmd->append_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, out_fd_spec) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s",
                fd, out_fd_spec, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: stdout now appending to '%s'", cmd->append_file);
    } else if (cmd->output_file) {
        int out_fd_spec = (cmd->output_fd > -1) ? cmd->output_fd : 1;
        LOG(LOG_LEVEL_INFO, "redir: open '%s' for truncate (fd=%d)",
            cmd->output_file, out_fd_spec);

        int fd = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            LOG(LOG_LEVEL_ERR, "redir: open('%s') failed: %s",
                cmd->output_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, out_fd_spec) < 0) {
            LOG(LOG_LEVEL_ERR, "redir: dup2(%d->%d) failed: %s",
                fd, out_fd_spec, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        LOG(LOG_LEVEL_INFO, "redir: stdout now truncating to '%s'", cmd->output_file);
    }

    // Optional: stderr redirection if you support 2> or fd-spec.
    // if (cmd->error_file) { ... dup2(..., 2) with LOG(...) }

    // Optional: heredoc already prepped into a pipe by parser/executor
    // If you carry cmd->heredoc as raw content, handle creation here.

    LOG(LOG_LEVEL_INFO, "redir: done");
    return 0;
}


//-----------------------------------------------------------------------------
// Existing free routines, unchanged
//-----------------------------------------------------------------------------
void free_command_list(Command **cmds, int num_cmds) {
    if (!cmds) return;
    for (int i = 0; i < num_cmds; ++i) {
        Command *cmd = cmds[i];
        if (cmd && cmd->argv && cmd->argv[0]) {
            LOG(LOG_LEVEL_INFO, "Freeing command[%d]: '%s'", i, cmd->argv[0]);
        } else {
            LOG(LOG_LEVEL_INFO, "Freeing command[%d]: <invalid>", i);
        }
        if (cmd) {
            free_command(cmd);
        }
    }
    free(cmds);
}

void free_command(Command *cmd) {
    if (!cmd) return;
    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; ++i) {
            free(cmd->argv[i]);
        }
        free(cmd->argv);
    }
    free(cmd->input_file);
    free(cmd->output_file);
    free(cmd->append_file);
    free(cmd->error_file);
    free(cmd->heredoc);
    free(cmd->cwd_override);
    free(cmd->raw_input);
    free(cmd);
}