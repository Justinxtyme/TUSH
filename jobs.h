#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>

void add_job(pid_t pgid, const char *cmdline);

int next_job_id(void);

#endif
