#ifndef APHELION_MODULE_H
#define APHELION_MODULE_H

#include "aphelion/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse and statically link root-relative source modules. Limits include the
 * entry: 256 modules, 64 active modules, 64 MiB aggregate source bytes.
 * Keep the returned tree alive while printing diagnostics: linked filenames
 * are owned by it. On load failure a failed-but-owned tree is returned whenever
 * possible. NULL means the initial parse could not allocate a program; in that
 * case diagnostics can borrow the caller's filename, which must remain alive. */
AstProgram *aphelion_module_parse(const char *filename, const char *source,
                                 Diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif
#endif
