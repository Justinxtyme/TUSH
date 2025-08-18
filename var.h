// var.h
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef VAR_H
#define VAR_H




typedef enum {
    V_NONE     = 0,
    V_EXPORT   = 1u << 0,
    V_READONLY = 1u << 1,
    V_INTEGER  = 1u << 2, // future
    V_SPECIAL  = 1u << 3  // e.g., PWD/OLDPWD
} VarFlags;

typedef struct Var {
    char *name;
    char *value;      // "" means set-but-empty; never NULL after creation
    uint32_t flags;
    struct Var *next; // pointer to next bucket head
} Var;

typedef struct VarTable {
    Var **buckets;
    size_t nbuckets;
    size_t count;     // number of entries
} VarTable;

// Lifecycle
bool vart_init(VarTable *t, size_t initial_buckets);  // e.g., 64
void vart_destroy(VarTable *t);

// Lookup
Var *vart_get(const VarTable *t, const char *name);

// Set/unset
bool is_var_assignment(const char *s);
bool vart_set(VarTable *t, const char *name, const char *value, uint32_t set_flags);
bool vart_unset(VarTable *t, const char *name);

// Flags
bool vart_export(VarTable *t, const char *name);
bool vart_unexport(VarTable *t, const char *name);

// Helpers
char **vart_build_envp(const VarTable *t); // malloc'd NULL-terminated array; caller frees

#endif // var.h