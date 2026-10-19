#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include <stdio.h>

/* Test-only ledger: no production allocation or global allocator changes. */
static void *live[65536];
static size_t live_count, attempts, failure_at;

void test_alloc_reset(size_t failure) {
    if (live_count != 0u) abort();
    attempts = 0u;
    failure_at = failure;
}
size_t test_alloc_attempts(void) { return attempts; }
size_t test_alloc_live(void) { return live_count; }
static int fail_now(void) { return ++attempts == failure_at; }
static void remember(void *pointer) {
    if (pointer == NULL) return;
    if (live_count >= sizeof(live) / sizeof(live[0])) abort();
    live[live_count++] = pointer;
}
static size_t locate(void *pointer) {
    size_t i;
    for (i = 0u; i < live_count; ++i) if (live[i] == pointer) return i;
    fputs("unowned or double free in allocator test\n", stderr);
    abort();
}
void *test_malloc(size_t size) {
    void *pointer;
    if (fail_now()) return NULL;
    pointer = malloc(size);
    remember(pointer);
    return pointer;
}
void *test_calloc(size_t count, size_t size) {
    void *pointer;
    if (fail_now()) return NULL;
    pointer = calloc(count, size);
    remember(pointer);
    return pointer;
}
void *test_realloc(void *pointer, size_t size) {
    int had_pointer = pointer != NULL;
    size_t index = had_pointer ? locate(pointer) : 0u;
    void *grown;
    if (fail_now()) return NULL;
    if (size == 0u) { test_free(pointer); return NULL; }
    grown = realloc(pointer, size);
    if (grown != NULL) {
        if (had_pointer) live[index] = grown; else remember(grown);
    }
    return grown;
}
void test_free(void *pointer) {
    size_t index;
    if (pointer == NULL) return;
    index = locate(pointer);
    live[index] = live[--live_count];
    free(pointer);
}
