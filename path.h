#ifndef PATH_H
#define PATH_H

int search_path_alloc(const char *cmd, char **outp);

bool has_slash(const char *s); 

bool is_directory(const char *path);

bool is_regular(const char *path);

bool is_executable(const char *path);

void print_exec_error(const char *what, int err);

#endif