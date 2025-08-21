/* this is an attempt to create a simple shell in C, called "THRASH", Totally Unnecessary Shell, Totally Useless Shell, or The Ultimate Shell.
 it will support basic built-in commands like 'cd', 'exit', and 'ls'   
 and other posix commands */

#include "input.h"
#include "shell.h" // Include the shell context and function declarations
#include "executor.h" // Include the command execution function
#include "jobs.h"
#include "history.h"
#include "var.h" 
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdio.h> // for printf, fgets, perror
#include <stdlib.h> // for exit,
#include <string.h> // for str maniopulation functions
#include "debug.h"
#include "signals.h"
#include <signal.h>
#include <unistd.h>



// --- Main Loop ---
int main() {
    ShellContext shell = { .running = 1 }; // Initialize shell context with running flag set to 1
    
    shell.vars = malloc(sizeof(VarTable));
    if (!shell.vars) {
        fprintf(stderr, "Failed to allocate VarTable\n");
        exit(1);
    }

    if (!vart_init(shell.vars, 64)) { // initialize var tables 
    LOG(LOG_LEVEL_ERR, "Failed to initialize VarTable");
    exit(EXIT_FAILURE);
    } 
    //init_var_table(&shell);
    setup_parent_signals(); 
    setup_shell_job_control(&shell);
    initialize_readline();
    
    //HISTORY SETUP
    char hist_path[4096];
    if (history_default_path(hist_path, sizeof(hist_path)) != 0) {
        // fallback if no $XDG_STATE_HOME / $HOME
        snprintf(hist_path, sizeof(hist_path), "history.txt");
    }

    if (history_init(&shell.history, hist_path, 2000,
                    HISTORY_IGNORE_EMPTY | HISTORY_IGNORE_DUPS | HISTORY_TRIM_TRAILING) != 0) {
        LOG(LOG_LEVEL_ERR, "Failed to init history: %s", strerror(errno));
    }

    if (history_load(&shell.history) != 0) {
        LOG(LOG_LEVEL_WARN, "No existing history loaded: %s", strerror(errno));
    }

    // Mirror into readline so Up-arrow / Ctrl+R see old entries
    for (size_t i = 0; i < history_count(&shell.history); ++i) {
        const HistEntry *e = get_history(&shell.history, i);
        if (e && e->line) add_history(e->line);
    }
    // Optional: limit readline’s in-memory size to match persistent cap
    //stifle_history((int)shell.history.max);
    
    // Log shell startup 
    LOG(LOG_LEVEL_INFO, "THRASH started, pid=%d", getpid());
    char *input_buf = NULL;
    bool continuation_mode = false;

    while (shell.running) {
        // Show normal cwd prompt or 🔪 continuation prompt
        if (!read_input(&shell, continuation_mode)) {  
            LOG(LOG_LEVEL_ERR, "read_input failed: %s", strerror(errno));
            perror("readline failed");
            break; // Ctrl+D or error
        }

        // Skip empty lines
        if (shell.input[0] == '\0') {
            continue;
        }

        // Append this chunk to the growing buffer
        append_to_buffer(&input_buf, shell.input);

        // Log just the chunk typed this round
        LOG(LOG_LEVEL_INFO, "logging: %s", shell.input);
        HistoryAddResult hr = history_add(&shell.history, shell.input);
        (void)hr;

        // Special-case: "$?" query — print and clear
        LOG(LOG_LEVEL_INFO, "checking for $?");
        if (strcmp(shell.input, "$?") == 0) {
            printf("%d\n", shell.last_status);
            free_buffer(&input_buf);
            continuation_mode = false;
            continue;
        }

        // Decide if ready to execute or still incomplete
        if (!is_command_complete(input_buf)) {
            continuation_mode = true;
            continue; // Loop again, show continuation prompt
        }

        // ✅ Command is complete — execute as usual
        continuation_mode = false;

        LOG(LOG_LEVEL_INFO, "expanding variables in: %s", input_buf);
        char *expanded = expand_variables_ex(input_buf, shell.last_status, shell.vars);
        LOG(LOG_LEVEL_INFO, "expanded=%s", expanded);
        if (!expanded) {
            perror("expand_variables");
            free_buffer(&input_buf);
            continue;
        }
        LOG(LOG_LEVEL_INFO, "Expanded input: '%s'", expanded);

        process_input_segments(&shell, expanded);

        free(expanded);
        free_buffer(&input_buf); // reset for next command
    }
    
    if (history_save(&shell.history) != 0) {
        LOG(LOG_LEVEL_WARN, "Failed to save history: %s", strerror(errno));
    }
    history_dispose(&shell.history);  // free internal buffers
    //cleanup_readline();
    vart_destroy(shell.vars);
    return 0;
}
