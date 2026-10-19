#ifndef APHELION_COMPILER_H
#define APHELION_COMPILER_H

/*
 * AST-to-bytecode compiler.
 *
 * The front end owns parsing and semantic validation.  The compiler consumes
 * the project's AstProgram directly; the VM never interprets source trees.
 */

#include "aphelion/bytecode.h"
#include "aphelion/ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AphelionSourceLocation {
    const char *file;
    uint32_t line;
    uint32_t column;
} AphelionSourceLocation;

typedef enum AphelionDiagnosticSeverity {
    APHELION_DIAGNOSTIC_ERROR = 0,
    APHELION_DIAGNOSTIC_WARNING
} AphelionDiagnosticSeverity;

typedef struct AphelionDiagnostic {
    AphelionDiagnosticSeverity severity;
    AphelionSourceLocation location;
    char *message;
} AphelionDiagnostic;

typedef struct AphelionDiagnostics {
    AphelionDiagnostic *items;
    size_t count;
    size_t capacity;
    bool allocation_failed;
} AphelionDiagnostics;

void aphelion_diagnostics_init(AphelionDiagnostics *diagnostics);
void aphelion_diagnostics_destroy(AphelionDiagnostics *diagnostics);
bool aphelion_diagnostics_add(AphelionDiagnostics *diagnostics,
                              AphelionDiagnosticSeverity severity,
                              AphelionSourceLocation location,
                              const char *message);
void aphelion_diagnostics_print(const AphelionDiagnostics *diagnostics,
                                FILE *out);
bool aphelion_diagnostics_has_errors(const AphelionDiagnostics *diagnostics);

typedef struct AphelionCompilerOptions {
    size_t max_functions;
    size_t max_constants;
    size_t max_globals;
    /* REPL-only echo of direct entry-level, non-silent expression statements. */
    bool echo_expressions;
} AphelionCompilerOptions;

void aphelion_compiler_options_default(AphelionCompilerOptions *options);

/* Compile only a validated AST.  On failure, diagnostics contains locations. */
bool aphelion_compile_ast(const AstProgram *program,
                          const AphelionCompilerOptions *options,
                          AphelionBytecode *out,
                          AphelionDiagnostics *diagnostics);

/* Primary front-end entry point for the project's AstProgram representation. */
bool aphelion_compile_program(const AstProgram *program,
                              const AphelionCompilerOptions *options,
                              AphelionBytecode *out,
                              AphelionDiagnostics *diagnostics);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_COMPILER_H */
