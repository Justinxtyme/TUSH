// history_test_debug.c
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h> // unlink

#ifdef HAVE_READLINE
#include <readline/history.h> // clear_history
#endif

static void clean_test_env(const char *path) {
    // Remove any persistent file from previous runs
    unlink(path);

    // Clear readline in-memory history if available
    #ifdef HAVE_READLINE
    clear_history();
    #endif
}

static void print_add_result(const char *label, HistoryAddResult r) {
    printf("[ADD] %s: id=%d, added_to_readline=%d\n",
           label, r.id, r.added_to_readline);
}

static void print_history_state(const History *h, const char *stage) {
    printf("[STATE] %s: count=%zu\n", stage, history_count(h));
    for (size_t i = 0; i < history_count(h); i++) {
        const HistEntry *e = get_history(h, i);
        if (e) {
            printf("  [%zu] id=%d line=\"%s\"\n", i, e->id, e->line);
        } else {
            printf("  [%zu] <null entry>\n", i);
        }
    }
}

int main(void) {
    History h;
    const char *test_path = "test_history.txt";

    clean_test_env(test_path);

    printf("=== INIT ===\n");
    assert(history_init(&h, test_path, 10,
       HISTORY_IGNORE_EMPTY | HISTORY_IGNORE_DUPS | HISTORY_TRIM_TRAILING | HISTORY_IGNORE_SPACE) == 0);
    print_history_state(&h, "after init");
    assert(history_count(&h) == 0);

    printf("\n=== ADD ENTRIES ===\n");
    HistoryAddResult r1 = history_add(&h, "echo hello");
    print_add_result("r1 (echo hello)", r1);

    HistoryAddResult r2 = history_add(&h, "pwd");
    print_add_result("r2 (pwd)", r2);

    HistoryAddResult r3 = history_add(&h, "echo hello"); 
    print_add_result("r3 (echo hello dup)", r3);

    HistoryAddResult r4 = history_add(&h, "echo hello"); // duplicate
    print_add_result("r4 (echo hello dup)", r4);

    print_history_state(&h, "after adds");

    printf("\n=== ASSERTIONS ===\n");
    if (!(r1.id != 0)) {
        fprintf(stderr, "FAIL: r1.id expected != 0, got %d\n", r1.id);
    }
    assert(r1.id != 0);

    if (!(r2.id != 0)) {
        fprintf(stderr, "FAIL: r2.id expected != 0, got %d\n", r2.id);
    }
    assert(r2.id != 0);

    if (!(r3.id != 0)) {
        fprintf(stderr, "FAIL: r3.id expected != 0 (duplicate ignored), got %d\n", r3.id);
    }
    assert(r3.id != 0);

    if (!(r4.id == 0)) {
        fprintf(stderr, "FAIL: r4.id expected == 0 (duplicate ignored), got %d\n", r4.id);
    }
    assert(r4.id == 0);

    if (!(history_count(&h) == 3)) {
        fprintf(stderr, "FAIL: expected history_count == 2, got %zu\n", history_count(&h));
    }
    assert(history_count(&h) == 3);

    printf("\n=== SAVE & RELOAD ===\n");
    assert(history_save(&h) == 0);
    history_dispose(&h);

    assert(history_init(&h, test_path, 10,
       HISTORY_IGNORE_EMPTY | HISTORY_IGNORE_DUPS | HISTORY_TRIM_TRAILING | HISTORY_IGNORE_SPACE) == 0);
    assert(history_load(&h) == 0);
    print_history_state(&h, "after reload");

    assert(history_count(&h) == 3);

    const HistEntry *e0 = get_history(&h, 0);
    const HistEntry *e1 = get_history(&h, 1);
    const HistEntry *e2 = get_history(&h, 2);

    if (!(e0 && strcmp(e0->line, "echo hello") == 0)) {
        fprintf(stderr, "FAIL: entry[0] expected \"echo hello\"\n");
    }
    assert(e0 && strcmp(e0->line, "echo hello") == 0);

    if (!(e1 && strcmp(e1->line, "pwd") == 0)) {
        fprintf(stderr, "FAIL: entry[1] expected \"pwd\"\n");
    }
    assert(e1 && strcmp(e1->line, "pwd") == 0);
    
    if (!(e2 && strcmp(e2->line, "echo hello") == 0)) {
        fprintf(stderr, "FAIL: entry[2] expected \"echo hello\"\n");
    }
    assert(e2 && strcmp(e2->line, "echo hello") == 0);

    printf("\n=== IGNORESPACE ===\n");
    size_t before = history_count(&h);
    HistoryAddResult rs = history_add(&h, "  ls -l"); // leading spaces
    print_add_result("rs (leading-space)", rs);
    assert(rs.id == 0);                  // should be ignored
    assert(history_count(&h) == before); // count unchanged



    history_dispose(&h);
    unlink(test_path);

    printf("\n✅ History module passed basic tests.\n");
    return 0;
}