#ifndef APHELION_PARSER_H
#define APHELION_PARSER_H

#include "aphelion/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse one source unit.  The returned tree is owned by the caller and must
 * be released with ast_program_free().  Diagnostics is intentionally opaque:
 * a caller may pass NULL when it only needs the tree, and the parser records
 * errors in AstProgram::had_parse_error as well.
 */
AstProgram *parser_parse(const char *filename, const char *source,
                         Diagnostics *diagnostics);

/* Spelling retained for clients which describe the operation as parsing a
 * source unit rather than a file. */
AstProgram *parser_parse_source(const char *filename, const char *source,
                                Diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_PARSER_H */
