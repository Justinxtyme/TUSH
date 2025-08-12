/* this is an attempt to create a simple shell in C, called "TUSH", Totally Unnecessary Shell, Totally Useless Shell, or The Ultimate Shell.
 it will support basic built-in commands like 'cd', 'exit', and 'ls'   
 and other posix commands */

#include "input.h"
#include "shell.h" // Include the shell context and function declarations
#include "executor.h" // Include the command execution function
#include "jobs.h"
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdio.h> // for printf, fgets, perror
#include <stdlib.h> // for exit,
#include <string.h> // for str maniopulation functions
#include "debug.h"
#include "signals.h"
#include <signal.h>


#ifdef _WIN32
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h> //// for POSIX functions like fork, execvp, chdir
#endif 



// --- Main Loop ---
int main() {
    ShellContext shell = { .running = 1 }; // Initialize shell context with running flag set to 1
    init_shell(&shell); // Initialize the shell context
    setup_parent_signals();
    initialize_readline();
    setup_shell_job_control(&shell);
    // Log shell startup 
    LOG(LOG_LEVEL_INFO, "TUSH started, pid=%d", getpid());
   
    while (shell.running) {
        //display_prompt(&shell); // Display the shell prompt

        if (!read_input(&shell)) {  
            LOG(LOG_LEVEL_ERR, "read_input failed: %s", strerror(errno));
            perror("readline failed");
            break; // Ctrl+D or error
        }

        add_to_history(&shell, shell.input); // adds input to history, for reuse

        int num_cmds = 0;
        char ***cmds = parse_pipeline(shell.input, &num_cmds);
        if (num_cmds == 0 || cmds == NULL || cmds[0] == NULL) continue;

        if (strcmp(cmds[0][0], "exit") == 0) {
            shell.running = 0;
        } else {
            int status = launch_pipeline(&shell, cmds, num_cmds);;



            // 1) Save for “$?” 
            shell.last_status = status;

            // 2) If stopped, add to jobs
            if (status == 128 + SIGTSTP) {
            add_job(shell.last_pgid, shell.input);
            fprintf(stderr, "[%d]+  Stopped  %s\n", next_job_id()-1, shell.input);
            }

            // 3) Maybe log it
            LOG(LOG_LEVEL_TRACE, "pipeline exited/stopped with %d", status);
        }

            // 4) free cmds…
    }
    cleanup_readline();
    return 0;
}
    

