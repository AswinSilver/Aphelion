#ifndef APHELION_LEXER_H
#define APHELION_LEXER_H

#include <stddef.h>

#include "aphelion/common.h"
#include "aphelion/token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lexer {
    const char *filename; /* borrowed; the caller keeps it alive */
    const char *source;   /* borrowed, NUL-terminated source text */
    const char *current;
    size_t offset;
    size_t line;
    size_t column;
    Diagnostics diagnostics;
} Lexer;

/* The source and filename remain owned by the caller for the Lexer's life. */
void lexer_init(Lexer *lexer, const char *filename, const char *source);
Token lexer_next(Lexer *lexer);

/* Releases only memory owned by the lexer (currently its diagnostics). */
void lexer_destroy(Lexer *lexer);
void lexer_dispose(Lexer *lexer);

const Diagnostics *lexer_diagnostics(const Lexer *lexer);
size_t lexer_diagnostic_count(const Lexer *lexer);
const Diagnostic *lexer_diagnostic_at(const Lexer *lexer, size_t index);
int lexer_has_errors(const Lexer *lexer);
void lexer_clear_diagnostics(Lexer *lexer);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_LEXER_H */
