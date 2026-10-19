#ifndef APHELION_COMMON_H
#define APHELION_COMMON_H

/*
 * Common source and diagnostic types shared by the lexer and later compiler
 * stages.  A filename is a borrowed pointer; the caller owns it.  Diagnostic
 * messages are owned by Diagnostics and are released by diagnostics_clear or
 * diagnostics_free.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SourceLocation {
    const char *filename;
    size_t offset; /* zero-based byte offset */
    size_t line;   /* one-based source line */
    size_t column; /* one-based byte column */
} SourceLocation;

typedef struct Diagnostic {
    SourceLocation location;
    char *message;
} Diagnostic;

typedef struct Diagnostics {
    Diagnostic *items;
    size_t count;
    size_t capacity;
    int allocation_failed; /* sticky even if an error message cannot be stored */
} Diagnostics;

void diagnostics_init(Diagnostics *diagnostics);
void diagnostics_clear(Diagnostics *diagnostics);
void diagnostics_free(Diagnostics *diagnostics);

/* Return non-zero on success, or zero if the diagnostic could not be stored. */
int diagnostics_add(Diagnostics *diagnostics,
                    SourceLocation location,
                    const char *message);
int diagnostics_addf(Diagnostics *diagnostics,
                     SourceLocation location,
                     const char *format,
                     ...);

size_t diagnostics_count(const Diagnostics *diagnostics);
const Diagnostic *diagnostics_at(const Diagnostics *diagnostics, size_t index);
int diagnostics_has_errors(const Diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_COMMON_H */
