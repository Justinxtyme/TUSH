// jobs.c
#include <stdlib.h>
#include <string.h>
#include "jobs.h"

typedef struct {
    int id;
    pid_t pgid;
    char *cmdline;
    int status; // 0 = running, 1 = stopped
} Job;

static Job job_table[64];
static int job_count = 0;

void add_job(pid_t pgid, const char *cmdline) {
    if (job_count >= 64) return;
    job_table[job_count].id = job_count + 1;
    job_table[job_count].pgid = pgid;
    job_table[job_count].cmdline = strdup(cmdline);
    job_table[job_count].status = 1;
    job_count++;
}

int next_job_id(void) {
    return job_count + 1;
}
