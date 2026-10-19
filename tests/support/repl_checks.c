#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include "aphelion/compiler.h"
#include "aphelion/module.h"
#include "aphelion/parser.h"
#include "aphelion/semantic.h"
#include "aphelion/vm.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

_Static_assert(APHELION_OP_REVEAL == 31 && APHELION_BYTECODE_VERSION == 1u,
               "REPL echo uses the existing bytecode format and opcode");

static void check_constant_ownership(const AphelionBytecode *bytecode) {
    size_t i;
    for (i = 0u; i < bytecode->constant_count; ++i) {
        if (bytecode->constants[i].type == APHELION_VALUE_TEXT)
            assert(bytecode->constants[i].as.text->references == 1u);
    }
}

static int execute(AphelionBytecode *bytecode, FILE *output, int quiet) {
    AphelionVM vm;
    int ok;
    aphelion_vm_init(&vm);
    /* A leaked expression result must fail even in relatively short programs. */
    aphelion_vm_set_limits(&vm, 32u, 32u);
    aphelion_vm_set_output(&vm, output);
    ok = aphelion_vm_run(&vm, bytecode);
    if (ok) {
        assert(vm.stack_count == 0u && vm.frame_count == 0u);
    } else if (!quiet) {
        const AphelionRuntimeError *error = aphelion_vm_error(&vm);
        fprintf(stderr, "%s:%" PRIu32 ":%" PRIu32 ": %s\n",
                error->file != NULL ? error->file : "<source>",
                error->line, error->column, error->message);
    }
    aphelion_vm_destroy(&vm);
    check_constant_ownership(bytecode);
    return ok;
}

static int pipeline(const char *filename, const char *source, const char *mode,
                    const char *save, size_t failure, int quiet) {
    Diagnostics diagnostics;
    AphelionDiagnostics compiler_diagnostics;
    AphelionCompilerOptions options;
    AphelionBytecode bytecode;
    AstProgram *program;
    FILE *output = quiet ? tmpfile() : stdout;
    char error[256] = {0};
    size_t i;
    int ok;
    int inspect = strstr(mode, "inspect") != NULL;
    int linked = strstr(mode, "linked") != NULL;
    if (output == NULL) return 0;
    test_alloc_reset(failure);
    diagnostics_init(&diagnostics);
    aphelion_diagnostics_init(&compiler_diagnostics);
    aphelion_bytecode_init(&bytecode);
    aphelion_compiler_options_default(&options);
    assert(!options.echo_expressions);
    options.echo_expressions = strcmp(mode, "--default") != 0 &&
                               strcmp(mode, "--default-inspect") != 0;
    program = linked ? aphelion_module_parse(filename, source, &diagnostics)
                     : parser_parse(filename, source, &diagnostics);
    ok = program != NULL && !program->had_parse_error &&
         !diagnostics_has_errors(&diagnostics) && semantic_check(program, &diagnostics);
    if (ok) {
        /* Exercise both public compiler entry points, including options=NULL. */
        ok = strcmp(mode, "--null") == 0
               ? aphelion_compile_ast(program, NULL, &bytecode, &compiler_diagnostics)
               : aphelion_compile_program(program, &options, &bytecode, &compiler_diagnostics);
    }
    if (!quiet) {
        for (i = 0u; i < diagnostics_count(&diagnostics); ++i) {
            const Diagnostic *item = diagnostics_at(&diagnostics, i);
            fprintf(stderr, "%s:%zu:%zu: %s\n", item->location.filename,
                    item->location.line, item->location.column, item->message);
        }
        aphelion_diagnostics_print(&compiler_diagnostics, stderr);
    }
    /* Diagnostics borrow AST filenames. Bytecode and its constants must outlive
     * the AST, including linked SOURCE filename constants and NUL-bearing text. */
    aphelion_diagnostics_destroy(&compiler_diagnostics);
    diagnostics_free(&diagnostics);
    ast_program_free(program);
    if (ok) {
        check_constant_ownership(&bytecode);
        ok = aphelion_bytecode_validate(&bytecode, error, sizeof(error));
        if (ok && save != NULL)
            ok = aphelion_bytecode_write_file(&bytecode, save, error, sizeof(error));
        if (!ok && !quiet) fprintf(stderr, "%s\n", error);
        if (ok) ok = inspect ? aphelion_bytecode_disassemble(&bytecode, output)
                             : execute(&bytecode, output, quiet);
    }
    aphelion_bytecode_destroy(&bytecode);
    if (quiet) assert(fclose(output) == 0);
    if (test_alloc_live() != 0u) {
        fprintf(stderr, "failure=%zu leaked=%zu\n", failure, test_alloc_live());
        abort();
    }
    return ok;
}

static int bytecode_run(const char *path, const char *save) {
    AphelionBytecode bytecode;
    char error[256] = {0};
    int ok;
    test_alloc_reset(0u);
    aphelion_bytecode_init(&bytecode);
    ok = aphelion_bytecode_read_file(&bytecode, path, error, sizeof(error));
    if (ok && save != NULL)
        ok = aphelion_bytecode_write_file(&bytecode, save, error, sizeof(error));
    if (ok) ok = execute(&bytecode, stdout, 0);
    else fprintf(stderr, "%s\n", error);
    aphelion_bytecode_destroy(&bytecode);
    assert(test_alloc_live() == 0u);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    FILE *file;
    char *source;
    long length;
    int ok;
    if (argc < 3 || argc > 4) return 2;
    if (strcmp(argv[1], "--bytecode") == 0)
        return bytecode_run(argv[2], argc == 4 ? argv[3] : NULL);
    file = fopen(argv[2], "rb");
    assert(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    assert(length >= 0L && fseek(file, 0L, SEEK_SET) == 0);
    source = malloc((size_t)length + 1u);
    assert(source != NULL);
    assert(fread(source, 1u, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    source[length] = '\0';
    if (strcmp(argv[1], "--sweep") == 0) {
        size_t point, attempts;
        ok = pipeline(argv[2], source, "--echo", NULL, 0u, 1);
        attempts = test_alloc_attempts();
        for (point = 1u; point <= attempts; ++point) {
            int result = pipeline(argv[2], source, "--echo", NULL, point, 1);
            assert(!result);
        }
        printf("REPL allocation cleanup checks passed (%zu failure points)\n", attempts);
    } else {
        ok = pipeline(argv[2], source, argv[1], argc == 4 ? argv[3] : NULL, 0u, 0);
    }
    free(source);
    return ok ? 0 : 1;
}
