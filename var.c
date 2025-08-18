/*================================================VAR.C========================================
===============================================================================================
 HEAVILY COMMENTED SO I CAN REMEMBER WHAT THE FUCK IS GOING ON
   var.c — implementation of shell variable table with export, readonly, and envp support
   Include the public interface for variable tables (types, flags, prototypes). */
#include "var.h"              // Header for VarTable, Var, flags, and function declarations
//  Bring in memory allocation and deallocation routines.
#include <stdlib.h>           // malloc, calloc, free
//  Bring in string manipulation functions used throughout this file.
#include <string.h>           // strdup, strcmp, strlen, memcpy, strcpy
//  errno is included for potential future error handling, but not referenced here.
#include <errno.h>            // errno (not used here but included)

#include "debug.h"
//  Define a 64-bit FNV-1a hash for fast, well-distributed string hashing.
//  Inline for performance; static to keep symbol local to this translation unit.
static inline uint64_t fnv1a64(const char *s) {
    //  Initialize with the FNV-1a offset basis constant for 64-bit.
    uint64_t h = 1469598103934665603ull; // FNV offset basis
    //  Iterate over each byte in the NUL-terminated string.
    for (; *s; ++s) {
        //  XOR the current byte into the hash; cast to unsigned to avoid sign-extension.
        h ^= (unsigned char)*s;          // XOR byte into hash
        //  Multiply by the 64-bit FNV prime to diffuse bits.
        h *= 1099511628211ull;           // FNV prime
    }
    //  Return the final 64-bit hash value.
    return h;
}

//  Compute the bucket index by masking the hash; nbuckets must be a power of two.
 // Compute bucket index from hash — assumes nbuckets is power-of-two
//  Inline for speed; static for internal linkage.
static inline size_t bucket_idx(const VarTable *t, const char *name) {
    //  Hash the name, then mask with nbuckets-1 to map into [0, nbuckets).
    return (size_t)(fnv1a64(name) & (t->nbuckets - 1)); // mask to bucket range
}

//  Wrapper around strdup that tolerates NULL by duplicating an empty string.
 // Safe strdup wrapper — handles NULL input by duplicating empty string
//  Static internal helper; returns heap-allocated copy.
static char *xstrdup(const char *s) {
    //  Ternary chooses "" if s is NULL; strdup allocates and copies.
    char *p = strdup(s ? s : ""); // fallback to ""
    //  Return the duplicated string (or NULL on allocation failure).
    return p;
}

//  Initialize the variable table with at least 16 buckets, rounding up to a power of two.
//  Returns true on success; false on invalid input or allocation failure.
bool vart_init(VarTable *t, size_t initial_buckets) {
    //  Validate the output pointer before doing any work.
    if (!t) return false;
    //  Start with the minimum bucket count.
    size_t n = 16;
    //  Increase n by powers of two until it's at least initial_buckets.
    while (n < initial_buckets) n <<= 1; // round up to power-of-two
    //  Allocate an array of bucket heads, zero-initialized for empty lists.
    t->buckets = calloc(n, sizeof(Var*)); // zero-initialized bucket array
    //  Abort on allocation failure.
    if (!t->buckets) return false;
    //  Record the number of buckets actually allocated.
    t->nbuckets = n;
    //  Initialize the number of stored variables to zero.
    t->count = 0;
    //  Signal successful initialization.
    return true;
}

//  Helper to free a Var and its owned name/value strings safely.
 // Free individual Var struct and its strings
//  Static internal because only used within this file.
static void free_var(Var *v) {
    //  Accept NULL and do nothing to simplify callers.
    if (!v) return;
    //  Free the heap-allocated variable name.
    free(v->name);
    //  Free the heap-allocated variable value.
    free(v->value);
    //  Free the Var struct itself.
    free(v);
}

//  Tear down an entire VarTable: free all nodes in all buckets, then the bucket array.
 // Destroy entire VarTable — free all Vars and bucket array
//  Safe to call on a partially initialized or already-destroyed table.
void vart_destroy(VarTable *t) {
    //  If t is NULL or buckets is NULL, there's nothing to free.
    if (!t || !t->buckets) return;
    //  Iterate through each bucket to free its linked list.
    for (size_t i = 0; i < t->nbuckets; ++i) {
        //  Start at the head of the list for this bucket.
        Var *v = t->buckets[i];
        //  Walk the list, freeing nodes one by one.
        while (v) {
            //  Preserve next pointer before freeing current node.
            Var *n = v->next; // save next before freeing
            //  Free the current Var (name, value, and struct).
            free_var(v);
            //  Advance to the next node.
            v = n;
        }
    }
    //  Free the array of bucket pointers itself.
    free(t->buckets);
    //  Null out the buckets pointer to avoid dangling references.
    t->buckets = NULL;
    //  Reset metadata to a consistent zeroed state.
    t->nbuckets = t->count = 0;
}

//  Find a variable by name, returning a pointer to its Var node or NULL if absent.
 // Lookup variable by name — returns pointer to Var or NULL
//  Constant table pointer allows read-only access.
Var *vart_get(const VarTable *t, const char *name) {
    //  Validate inputs: need a table and a non-NULL name.
    if (!t || !name) return NULL;
    //  Traverse the appropriate bucket list computed from the name.
    for (Var *v = t->buckets[bucket_idx(t, name)]; v; v = v->next)
        //  Compare the current node's name to the lookup key.
        if (strcmp(v->name, name) == 0) return v;
    //  Name not found in the bucket chain.
    return NULL;
}

//  Grow the hash table when load factor exceeds 0.75 by doubling bucket count.
 // Resize table if load factor exceeds 0.75 — doubles bucket count
//  Returns true if no resize needed or if resize succeeds; false on allocation failure.
static bool maybe_resize(VarTable *t) {
    //  If count/nbuckets < 0.75, skip resizing; multiplied form avoids floating point.
    if ((t->count * 4) < (t->nbuckets * 3)) return true; // load < 0.75
    //  Compute the new bucket count by doubling the current count.
    size_t newn = t->nbuckets << 1;
    //  Allocate a new, zeroed array for the resized bucket table.
    Var **newb = calloc(newn, sizeof(Var*));
    //  Bail out if the allocation fails; table remains unchanged.
    if (!newb) return false;

    //  Rehash all existing entries into the new bucket array.
    // Rehash all variables into new bucket array
    //  Iterate over each old bucket to migrate its list.
    for (size_t i = 0; i < t->nbuckets; ++i) {
        //  Start at the head of the old list for bucket i.
        Var *v = t->buckets[i];
        //  Walk the old list, moving nodes into the new array.
        while (v) {
            //  Save next pointer before relinking v into new buckets.
            Var *next = v->next;
            //  Compute the new bucket index using the same hash and mask with newn-1.
            size_t idx = (size_t)(fnv1a64(v->name) & (newn - 1));
            //  Insert node at the head of the new bucket's list to avoid extra traversal.
            v->next = newb[idx];
            //  Update the new bucket head to point to v.
            newb[idx] = v;
            //  Advance to the next node from the old list.
            v = next;
        }
    }

    //  Free the old bucket array now that nodes have been moved.
    free(t->buckets);
    //  Install the new bucket array into the table.
    t->buckets = newb;
    //  Record the new bucket count.
    t->nbuckets = newn;
    //  Report success after resizing.
    return true;
}

//  Create or update a variable; enforces readonly, merges flags, and triggers resize if needed.
 // Set or update a variable — handles readonly, export, and resizing
//  set_flags may include bits like V_EXPORT and V_READONLY (if you allow setting it on creation).
bool vart_set(VarTable *t, const char *name, const char *value, uint32_t set_flags) {
    //  Validate required inputs: table and name must be non-NULL.
    if (!t || !name) return false;

    //  Enforce shell variable naming: [A-Za-z_][A-Za-z0-9_]*
    // Validate variable name: must match [A-Za-z_][A-Za-z0-9_]*
    //  First character must be a letter or underscore.
    if (!( (name[0]=='_' ) || ( (name[0]>='A'&&name[0]<='Z') || (name[0]>='a'&&name[0]<='z') )))
        return false;
    //  Subsequent characters may be letters, digits, or underscore.
    for (const char *p = name+1; *p; ++p)
        if (!(*p=='_' || (*p>='A'&&*p<='Z') || (*p>='a'&&*p<='z') || (*p>='0'&&*p<='9'))) return false;

    //  Compute the target bucket for this name.
    size_t idx = bucket_idx(t, name);
    //  Scan the chain to see if the variable already exists.
    for (Var *v = t->buckets[idx]; v; v = v->next) {
        //  Compare names for equality; strcmp == 0 means a match.
        if (strcmp(v->name, name) == 0) {
            //  Refuse to modify variables marked readonly.
            if (v->flags & V_READONLY) return false; // can't modify readonly
            //  If a value was provided, duplicate and replace the existing value.
            if (value) {
                //  Duplicate the new value string; may return NULL on OOM.
                char *nv = xstrdup(value);
                //  Propagate failure if duplication failed.
                if (!nv) return false;
                //  Free the previous heap-allocated value.
                free(v->value);
                //  Install the new value pointer.
                v->value = nv;
            } else {
                //  Treat NULL value as setting an empty string (like sh behavior).
                free(v->value);
                v->value = xstrdup(""); // treat NULL as empty string
            }
            //  Merge new flags into existing flags (bitwise OR).
            v->flags |= set_flags; // merge flags (e.g. preserve export)
            //  Done updating; return success.
            return true;
        }
    }

    //  Not found in table; create a new Var node.
    // Not found — create new Var
    //  Allocate a zero-initialized Var to ensure next is NULL and flags start at 0.
    Var *nv = calloc(1, sizeof(Var));
    //  Abort on allocation failure.
    if (!nv) return false;
    //  Duplicate and assign the variable name; store heap pointer in nv->name.
    nv->name = xstrdup(name);
    //  Duplicate and assign the value; use empty string if value is NULL.
    nv->value = xstrdup(value ? value : "");
    //  Initialize flags with the provided set_flags (e.g., V_EXPORT).
    nv->flags = set_flags;
    //  Insert at the head of the appropriate bucket's singly linked list.
    nv->next = t->buckets[idx]; // insert at head
    //  Update the bucket head to point to the new node.
    t->buckets[idx] = nv;
    //  Increment the element count used for load factor and size decisions.
    t->count++;
    //  Possibly resize the table; return its result (true on success).
    return maybe_resize(t); // resize if needed
}

//  Remove a variable by name, unless it is marked readonly.
 // Unset (delete) a variable — respects readonly
//  Returns true if deleted; false if not found or readonly.
bool vart_unset(VarTable *t, const char *name) {
    //  Validate inputs.
    if (!t || !name) return false;
    //  Compute the bucket containing the target (if present).
    size_t idx = bucket_idx(t, name);
    //  Use a pointer-to-pointer to easily unlink from a singly linked list.
    Var **pp = &t->buckets[idx]; // pointer-to-pointer for unlinking
    //  Traverse the list, keeping pp pointing to the current next field.
    for (Var *v = *pp; v; v = v->next) {
        //  Check for a name match on the current node.
        if (strcmp(v->name, name) == 0) {
            //  Refuse to delete readonly variables.
            if (v->flags & V_READONLY) return false;
            //  Unlink v by updating the previous next-pointer (or bucket head).
            *pp = v->next; // unlink
            //  Free the removed node and its strings.
            free_var(v);
            //  Decrement the table's element count.
            t->count--;
            //  Report successful deletion.
            return true;
        }
        //  Advance pp so it points to the next field of the current node.
        pp = &v->next;
    }
    //  Name not found in the table.
    return false;
}

//  Mark a variable as exported; create it empty if it doesn't exist (bash-like behavior).
 // Mark variable as exported — creates empty var if not found
//  Returns true on success; false on invalid input or allocation failure in creation path.
bool vart_export(VarTable *t, const char *name) {
    //  Validate inputs.
    if (!t || !name) return false;
    //  Attempt to locate an existing variable.
    Var *v = vart_get(t, name);
    //  If not present, create it with empty string and export flag.
    if (!v) {
        //  Mirrors `export FOO` creating FOO="" in bash.
        // bash behavior: `export FOO` creates FOO=""
        return vart_set(t, name, "", V_EXPORT);
    }
    //  Set the export flag bit on the existing variable.
    v->flags |= V_EXPORT;
    //  Report success.
    return true;
}

//  Clear the export flag on an existing variable; no-op if not found.
 // Remove export flag
//  Returns true if the variable exists (flag cleared), false if not found.
bool vart_unexport(VarTable *t, const char *name) {
    //  Look up the variable; table pointer assumed valid by caller.
    Var *v = vart_get(t, name);
    //  If the variable doesn't exist, report failure.
    if (!v) return false;
    //  Clear the export bit without affecting other flags.
    v->flags &= ~V_EXPORT;
    //  Report success.
    return true;
}

//  Build a freshly allocated envp[] from the exported variables in the table.
//  Caller takes ownership of the returned array and each string within it.
//  Returns NULL on allocation failure or if 't' is NULL.
//  Build a freshly allocated envp[] from the exported variables in the table.
//  Caller owns the returned array and each string within it.
//  Returns NULL on allocation failure or if 't' is NULL.
char **vart_build_envp(const VarTable *t) {
    //  Validate input; cannot build from a NULL table.
    LOG(LOG_LEVEL_INFO, "Building envp");
    if (!t) {
         LOG(LOG_LEVEL_ERR, "NULL TABLE");
        return NULL;
    }

    //  First pass: count how many variables are marked for export.
    //  Iterate across all buckets to count exported entries.
    size_t n = 0;
    if (t->nbuckets == 0 || !t->buckets) {
        //  No buckets: return an empty envp array (just the NULL terminator).
        char **empty = calloc(1, sizeof(char *));
        return empty; // empty envp is valid for execve
    }
    for (size_t i = 0; i < t->nbuckets; ++i)
        //  Walk the linked list in bucket i.
        for (Var *v = t->buckets[i]; v; v = v->next)
            //  Increment count for variables with V_EXPORT set and non-empty name.
            if ((v->flags & V_EXPORT) && v->name && v->name[0] != '\0') n++;

    //  Allocate envp array of n pointers plus a terminating NULL slot.
    char **envp = calloc(n + 1, sizeof(char*)); // +1 for NULL terminator
    //  Allocation failure yields NULL; caller must handle.
    if (!envp) return NULL;

    //  k indexes into envp as we populate it in the second pass.
    size_t k = 0;
    //  Second pass: fill envp with "NAME=VALUE" strings for exported vars.
    for (size_t i = 0; i < t->nbuckets; ++i) {
        //  Iterate each variable in the current bucket.
        for (Var *v = t->buckets[i]; v; v = v->next) {
            //  Skip non-exported or invalid-name variables.
            if (!(v->flags & V_EXPORT)) continue;
            if (!v->name || v->name[0] == '\0') continue;

            //  Treat NULL value as empty string to avoid segfaults.
            const char *val = v->value ? v->value : "";

            //  Compute length: name + '=' + value + terminating NUL.
            size_t namelen = strlen(v->name);
            size_t vallen  = strlen(val);
            size_t len = namelen + 1 + vallen + 1; // NAME=VALUE\0

            //  Allocate a buffer for the formatted NAME=VALUE string.
            char *s = malloc(len);
            //  If allocation fails, free any previously allocated strings and the envp array.
            if (!s) {
                //  Best-effort cleanup of entries that were already created.
                for (size_t j = 0; j < k; ++j) free(envp[j]); // free each populated "NAME=VALUE"
                free(envp);                                  // free the envp pointer array
                return NULL;                                 // propagate failure
            }

            //  Copy the variable name into the buffer (without the terminating NUL).
            memcpy(s, v->name, namelen);
            //  Place the '=' separator directly after the name.
            s[namelen] = '=';
            //  Copy the variable value (including terminating NUL) right after '='.
            memcpy(s + namelen + 1, val, vallen + 1);

            //  Assign the assembled string into the envp array.
            envp[k++] = s;
        }
    }

    //  Null-terminate the envp array so exec-family calls know where it ends.
    envp[k] = NULL;

    //  Return the newly built environment pointer array to the caller.
    return envp;
}