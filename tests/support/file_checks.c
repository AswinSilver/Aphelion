#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include "aphelion/bytecode.h"
#include "aphelion/file.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char payload[] = {0u, '\r', '\n', 0x1au, 0xffu, 'x'};

/* argv is deliberately ASCII: the driver tests the UTF-8 API independently
 * of the Windows CRT's ANSI argv decoding. */
static char *decode_path(const char *hex) {
    size_t i, length = strlen(hex);
    char *path;
    assert(length % 2u == 0u);
    path = (char *)malloc(length / 2u + 1u);
    assert(path != NULL);
    for (i = 0u; i < length; i += 2u) {
        unsigned byte;
        assert(sscanf(hex + i, "%2x", &byte) == 1);
        assert(byte != 0u);
        path[i / 2u] = (char)byte;
    }
    path[length / 2u] = '\0';
    return path;
}

static void check_errors(void) {
    errno = 0;
    assert(aphelion_file_open(NULL, "rb") == NULL && errno == EINVAL);
    errno = 0;
    assert(aphelion_file_open("unused", NULL) == NULL && errno == EINVAL);
    errno = 0;
    assert(!aphelion_file_is_regular(NULL) && errno == EINVAL);
    errno = 0;
    assert(aphelion_file_open("__missing_file_fixture__.apb", "rb") == NULL);
    assert(errno == ENOENT);
    errno = 0;
    assert(!aphelion_file_is_regular("__missing_file_fixture__.apb"));
    assert(errno == ENOENT);
#ifdef _WIN32
    {
        const char *invalid[] = {"\xc3", "\xc0\xaf", "\xed\xa0\x80",
                                 "\xf4\x90\x80\x80", "\xe2\x28\xa1"};
        size_t i;
        for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            errno = 0;
            assert(aphelion_file_open(invalid[i], "rb") == NULL);
            assert(errno == EILSEQ && test_alloc_live() == 0u);
            errno = 0;
            assert(!aphelion_file_is_regular(invalid[i]));
            assert(errno == EILSEQ && test_alloc_live() == 0u);
        }
        errno = 0;
        assert(aphelion_file_open("unused", "\xc3") == NULL);
        assert(errno == EILSEQ && test_alloc_live() == 0u);
    }
#endif
}

static void check_allocations(const char *path) {
    size_t failure, attempts;
    FILE *file;
    test_alloc_reset(0u);
    file = aphelion_file_open(path, "rb");
    assert(file != NULL);
    attempts = test_alloc_attempts();
    assert(fclose(file) == 0 && test_alloc_live() == 0u);
#ifdef _WIN32
    assert(attempts == 2u);
#endif
    for (failure = 1u; failure <= attempts; ++failure) {
        test_alloc_reset(failure);
        errno = 0;
        assert(aphelion_file_open(path, "rb") == NULL);
        assert(errno == ENOMEM && test_alloc_live() == 0u);
        /* A failed conversion must not truncate an existing output file. */
        test_alloc_reset(failure);
        errno = 0;
        assert(aphelion_file_open(path, "wb") == NULL);
        assert(errno == ENOMEM && test_alloc_live() == 0u);
    }
    test_alloc_reset(0u);
    assert(aphelion_file_is_regular(path));
    attempts = test_alloc_attempts();
    assert(test_alloc_live() == 0u);
#ifdef _WIN32
    assert(attempts == 1u);
#endif
    for (failure = 1u; failure <= attempts; ++failure) {
        test_alloc_reset(failure);
        errno = 0;
        assert(!aphelion_file_is_regular(path));
        assert(errno == ENOMEM && test_alloc_live() == 0u);
    }
}

static void check_bytecode(const char *path) {
    AphelionBytecode original, loaded;
    char error[512];
    aphelion_bytecode_init(&original);
    aphelion_bytecode_init(&loaded);
    assert(aphelion_bytecode_set_source_name(&original, path));
    assert(aphelion_bytecode_add_function(&original, "main", 0u, 0u) == 0);
    assert(aphelion_function_emit(&original.functions[0], APHELION_OP_HALT,
                                  0, 0, 1u, 1u) == 0);
    assert(aphelion_bytecode_write_file(&original, path, error, sizeof(error)));
    assert(aphelion_file_is_regular(path));
    assert(aphelion_bytecode_read_file(&loaded, path, error, sizeof(error)));
    assert(loaded.function_count == 1u && loaded.functions[0].code_count == 1u);
    assert(loaded.functions[0].code[0].opcode == APHELION_OP_HALT);
    assert(strcmp(loaded.source_name, path) == 0);
    aphelion_bytecode_destroy(&loaded);
    aphelion_bytecode_destroy(&original);
}

int main(int argc, char **argv) {
    char *path = NULL;
    FILE *file;
    test_alloc_reset(0u);
    assert(argc == 2 || argc == 3);
    if (argc == 3) path = decode_path(argv[2]);
    if (strcmp(argv[1], "errors") == 0) {
        check_errors();
    } else {
        assert(path != NULL);
        if (strcmp(argv[1], "read") == 0) {
            unsigned char actual[sizeof(payload)];
            file = aphelion_file_open(path, "rb");
            assert(file != NULL);
            assert(fread(actual, 1u, sizeof(actual), file) == sizeof(actual));
            assert(memcmp(actual, payload, sizeof(payload)) == 0);
            assert(fgetc(file) == EOF && !ferror(file));
            assert(fclose(file) == 0);
        } else if (strcmp(argv[1], "write") == 0) {
            file = aphelion_file_open(path, "wb");
            assert(file != NULL);
            assert(fwrite(payload, 1u, sizeof(payload), file) == sizeof(payload));
            assert(fclose(file) == 0);
        } else if (strcmp(argv[1], "regular") == 0) {
            assert(aphelion_file_is_regular(path));
        } else if (strcmp(argv[1], "not-regular") == 0) {
            assert(!aphelion_file_is_regular(path));
        } else if (strcmp(argv[1], "alloc") == 0) {
            check_allocations(path);
        } else {
            assert(strcmp(argv[1], "bytecode") == 0);
            check_bytecode(path);
        }
    }
    free(path);
    assert(test_alloc_live() == 0u);
    puts("file checks passed");
    return 0;
}
