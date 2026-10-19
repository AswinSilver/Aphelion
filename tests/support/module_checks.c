#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include "aphelion/module.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int pipeline(const char *filename, const char *source, size_t failure) {
    Diagnostics diagnostics;
    AstProgram *program;
    size_t i;
    int ok;
    test_alloc_reset(failure);
    diagnostics_init(&diagnostics);
    program = aphelion_module_parse(filename, source, &diagnostics);
    ok = program != NULL && !program->had_parse_error && !diagnostics_has_errors(&diagnostics);
    /* Consume borrowed filenames before releasing their owner, on failure too.
     * ASan makes this an ownership check, not just a textual result check. */
    for (i = 0u; i < diagnostics_count(&diagnostics); ++i) {
        const Diagnostic *diagnostic = diagnostics_at(&diagnostics, i);
        assert(diagnostic->location.filename != NULL);
        assert(strlen(diagnostic->location.filename) != 0u);
        assert(diagnostic->message != NULL);
        if (failure == 0u) fprintf(stderr, "%s:%zu: %s\n",
            diagnostic->location.filename, diagnostic->location.line, diagnostic->message);
    }
    if (program != NULL) {
        for (i = 0u; i < program->source_file_count; ++i)
            assert(strlen(program->source_files[i]) != 0u);
        for (i = 0u; i < program->item_count; ++i) {
            const AstTopLevel *item = &program->items[i];
            assert(item->span.start.filename != NULL);
            assert(strlen(item->span.start.filename) != 0u);
        }
    }
    diagnostics_free(&diagnostics);
    ast_program_free(program);
    assert(test_alloc_live() == 0u);
    return ok;
}

int main(int argc, char **argv) {
    FILE *file;
    long length;
    char *source;
    size_t attempts, failure;
    int ok;
    if (argc != 2 && argc != 3) return 2;
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    assert(length >= 0L && fseek(file, 0L, SEEK_SET) == 0);
    source = malloc((size_t)length + 1u);
    assert(source != NULL);
    assert(fread(source, 1u, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    source[length] = '\0';
    ok = pipeline(argv[1], source, 0u);
    attempts = test_alloc_attempts();
    if (argc == 3 && strcmp(argv[2], "--sweep") == 0) {
        for (failure = 1u; failure <= attempts; ++failure)
            assert(!pipeline(argv[1], source, failure));
        printf("module loader allocation cleanup checks passed (%zu failure points)\n", attempts);
    }
    free(source);
    return ok ? 0 : 1;
}
