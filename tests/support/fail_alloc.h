#ifndef APHELION_TEST_FAIL_ALLOC_H
#define APHELION_TEST_FAIL_ALLOC_H
#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include <stddef.h>
#include <stdlib.h>

void *test_malloc(size_t size);
void *test_calloc(size_t count, size_t size);
void *test_realloc(void *pointer, size_t size);
void test_free(void *pointer);
void test_alloc_reset(size_t failure);
size_t test_alloc_attempts(void);
size_t test_alloc_live(void);

#ifndef APHELION_ALLOC_IMPLEMENTATION
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#endif
#endif
