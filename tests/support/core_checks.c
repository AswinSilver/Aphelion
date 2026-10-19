#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include "aphelion/parser.h"
#include "aphelion/semantic.h"
#include "aphelion/compiler.h"
#include "aphelion/vm.h"
#include <stdio.h>
#include <string.h>

static const char *valid =
    "forge global: decimal = 5\n"
    "orbit f(value: text, n: whole) -> text {\n"
    " when n <= 0 { yield value } otherwise { yield f(value + \"x\", n - 1) }\n"
    "}\n"
    "reveal(f(\"hello\", 3))\n"
    "reveal(to_text(receive()))\n";

static int pipeline(const char *source, size_t failure) {
    Diagnostics diagnostics;
    AphelionDiagnostics compiler_diagnostics;
    AstProgram *program;
    AphelionBytecode bytecode;
    AphelionVM vm;
    FILE *input = tmpfile(), *output = tmpfile();
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return 1;
    }
    fwrite("a\0b\n", 1u, 4u, input);
    rewind(input);
    test_alloc_reset(failure);
    diagnostics_init(&diagnostics);
    aphelion_diagnostics_init(&compiler_diagnostics);
    aphelion_bytecode_init(&bytecode);
    aphelion_vm_init(&vm);
    aphelion_vm_set_input(&vm, input);
    aphelion_vm_set_output(&vm, output);
    program = parser_parse("allocation.agne", source, &diagnostics);
    if (program != NULL && !program->had_parse_error &&
        !diagnostics_has_errors(&diagnostics) && semantic_check(program, &diagnostics)) {
        if (aphelion_compile_program(program, NULL, &bytecode, &compiler_diagnostics))
            (void)aphelion_vm_run(&vm, &bytecode);
    }
    aphelion_vm_destroy(&vm);
    aphelion_bytecode_destroy(&bytecode);
    ast_program_free(program);
    diagnostics_free(&diagnostics);
    aphelion_diagnostics_destroy(&compiler_diagnostics);
    fclose(input);
    fclose(output);
    if (test_alloc_live() != 0u) {
        fprintf(stderr, "failure=%zu leaked=%zu\n", failure, test_alloc_live());
        return 1;
    }
    return 0;
}

static int allocations(void) {
    const char *sources[] = {valid, "forge whole = ;", "orbit f(whole) -> whole { yield }",
        "reveal(1 + 2 * (3 + 4))\n", "forge a = missing\n", "reveal(\"bad\\q\")\n",
        "shift xs = [\"a\", \"b\"]\n"
        "traverse xs in xs { when xs == \"a\" { skip } reveal(xs) }\n"
        "traverse item in xs { xs = [\"new\"]; traverse item in xs { escape } }\n"
        "forge empty: [text] = []\ntraverse item in empty { reveal(item) }\n"
        "orbit first(xs: [text]) -> text {\n"
        " traverse item in xs { yield item }\n yield \"empty\"\n}\n"
        "reveal(first([\"return\"]))\n",
        "orbit fail(xs: [text]) -> silent {\n"
        " traverse item in xs { assert(dim, item) }\n yield\n}\n"
        "fail([\"owned failure\\0tail\"])\n",
        "traverse item in [\"owned\"] { traverse inner in missing { reveal(item) } }\n"};
    size_t i, point, attempts;
    for (point = 1u; point <= 2u; ++point) {
        Diagnostics diagnostics;
        AphelionDiagnostics compiler_diagnostics;
        test_alloc_reset(point);
        diagnostics_init(&diagnostics);
        (void)diagnostics_add(&diagnostics, (SourceLocation){"oom", 0u, 1u, 1u}, "error");
        if (!diagnostics_has_errors(&diagnostics)) return 1;
        diagnostics_free(&diagnostics);
        if (test_alloc_live() != 0u) return 1;
        test_alloc_reset(point);
        aphelion_diagnostics_init(&compiler_diagnostics);
        (void)aphelion_diagnostics_add(&compiler_diagnostics, APHELION_DIAGNOSTIC_ERROR,
                                       (AphelionSourceLocation){"oom", 1u, 1u}, "error");
        if (!aphelion_diagnostics_has_errors(&compiler_diagnostics)) return 1;
        aphelion_diagnostics_destroy(&compiler_diagnostics);
        if (test_alloc_live() != 0u) return 1;
    }
    for (i = 0u; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        if (pipeline(sources[i], 0u)) return 1;
        attempts = test_alloc_attempts();
        for (point = 1u; point <= attempts; ++point) {
            fprintf(stderr, "allocation scenario=%zu point=%zu\n", i, point);
            if (pipeline(sources[i], point)) return 1;
        }
    }
    puts("allocation cleanup checks passed");
    return 0;
}

static int bytecode(const char *path, const char *output_path) {
    AphelionBytecode program;
    AphelionVM vm;
    char error[256];
    int ok;
    test_alloc_reset(0u);
    aphelion_bytecode_init(&program);
    aphelion_vm_init(&vm);
    aphelion_vm_set_instruction_limit(&vm, 1000u);
    ok = aphelion_bytecode_read_file(&program, path, error, sizeof(error));
    if (ok) {
        if (output_path != NULL &&
            !aphelion_bytecode_write_file(&program, output_path, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            ok = 0;
        }
    }
    if (ok) {
        ok = aphelion_vm_run(&vm, &program);
        if (!ok) fprintf(stderr, "%s\n", aphelion_vm_error(&vm)->message);
    } else fprintf(stderr, "%s\n", error);
    aphelion_vm_destroy(&vm);
    aphelion_bytecode_destroy(&program);
    if (test_alloc_live() != 0u) return 2;
    return ok ? 0 : 1;
}

static int allocation_read(const char *path) {
    size_t point, count = 0u;
    for (point = 0u; point <= count; ++point) {
        AphelionBytecode program;
        char error[256] = {0};
        FILE *output = tmpfile();
        int ok;
        if (output == NULL) return 1;
        test_alloc_reset(point);
        aphelion_bytecode_init(&program);
        ok = aphelion_bytecode_read_file(&program, path, error, sizeof(error));
        if (ok) ok = aphelion_bytecode_disassemble(&program, output);
        if (fclose(output) != 0) ok = 0;
        aphelion_bytecode_destroy(&program);
        if (point == 0u) {
            if (!ok) {
                fprintf(stderr, "baseline loader/disassembly failed: %s\n", error);
                return 1;
            }
            count = test_alloc_attempts();
        } else if (point <= test_alloc_attempts() && ok) {
            fprintf(stderr, "allocation %zu was masked as successful disassembly\n", point);
            return 1;
        }
        if (test_alloc_live() != 0u) {
            fprintf(stderr, "loader allocation %zu leaked %zu objects\n", point, test_alloc_live());
            return 2;
        }
    }
    puts("loader allocation checks passed (including disassembly)");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return allocations();
    if (argc == 3 && strcmp(argv[1], "--alloc-read") == 0) return allocation_read(argv[2]);
    return bytecode(argv[1], argc == 3 ? argv[2] : NULL);
}
