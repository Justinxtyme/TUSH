#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    History h;
    const char *test_path = "test_history.txt";

    // Initialize
    assert(history_init(&h, test_path, 10, HISTORY_IGNORE_EMPTY | HISTORY_IGNORE_DUPS | HISTORY_TRIM_TRAILING) == 0);
    assert(history_count(&h) == 0);

    // Add entries
    HistoryAddResult r1 = history_add(&h, "echo hello");
    HistoryAddResult r2 = history_add(&h, "pwd");
    HistoryAddResult r3 = history_add(&h, "echo hello"); // duplicate

    assert(r1.id != 0);
    assert(r2.id != 0);
    assert(r3.id == 0); // should be ignored due to HISTORY_IGNORE_DUPS

    assert(history_count(&h) == 2);

    // Save
    assert(history_save(&h) == 0);
    history_dispose(&h);

    // Reload
    assert(history_init(&h, test_path, 10, 0) == 0);
    assert(history_load(&h) == 0);
    assert(history_count(&h) == 2);

    const HistEntry *e0 = get_history(&h, 0);
    const HistEntry *e1 = get_history(&h, 1);

    assert(e0 && strcmp(e0->line, "echo hello") == 0);
    assert(e1 && strcmp(e1->line, "pwd") == 0);

    history_dispose(&h);
    printf("✅ History module passed basic tests.\n");
    return 0;
}