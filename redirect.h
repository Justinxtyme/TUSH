//redirect.h
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include "command.h"
 #ifndef REDIRECT_H
#define REDIRECT_H
 

typedef enum {
    REDIR_IN, REDIR_OUT, REDIR_APPEND, REDIR_ERR,
    REDIR_DUP_OUT, REDIR_DUP_ERR,
    REDIR_HEREDOC, REDIR_CWD
} RedirType;

typedef struct {
    RedirType type;
    int target_fd;       // FD being redirected
    int source_fd;       // For dup2-style redirs
    char *filename;      // For file-based redirs
    char *heredoc_data;  // For heredoc
} Redirection;



int perform_redirections(Command *cmd);


#endif
