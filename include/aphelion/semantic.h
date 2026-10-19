#ifndef APHELION_SEMANTIC_H
#define APHELION_SEMANTIC_H

#include "aphelion/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve names, infer expression types, and validate control flow. */
bool semantic_check(AstProgram *program, Diagnostics *diagnostics);

/* Assignment/argument compatibility used by the compiler and integrations. */
bool semantic_types_compatible(TypeKind expected, TypeKind actual);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_SEMANTIC_H */
