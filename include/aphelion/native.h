#ifndef APHELION_NATIVE_H
#define APHELION_NATIVE_H

/*
 * Native modules are compiled into the runtime and imported with `summon`.
 * The front end resolves `acsl.sha256(...)` against this registry; the VM
 * performs the final argument validation before invoking the implementation.
 *
 * A native function never owns its arguments.  On success it stores a new
 * owned value in *result; on failure it returns false and leaves *result
 * untouched while writing a short, secret-free diagnostic to error.
 */

#include "aphelion/ast.h"
#include "aphelion/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APHELION_NATIVE_MAX_ARITY 8u

typedef enum AphelionNativeParam {
    APHELION_NATIVE_PARAM_TEXT = 0,
    APHELION_NATIVE_PARAM_WHOLE,
    APHELION_NATIVE_PARAM_SIGNAL,
    APHELION_NATIVE_PARAM_ANY
} AphelionNativeParam;

typedef bool (*AphelionNativeInvoke)(const AphelionValue *arguments,
                                     size_t argument_count,
                                     AphelionValue *result,
                                     char *error, size_t error_size);

typedef struct AphelionNativeFunction {
    const char *module;
    const char *name;
    const char *signature; /* compact source-level signature for diagnostics */
    unsigned arity;
    AphelionNativeParam parameters[APHELION_NATIVE_MAX_ARITY];
    TypeKind return_type;
    AphelionNativeInvoke invoke;
} AphelionNativeFunction;

/* A native module name is recognised without touching the filesystem.  This
 * helper is deliberately inline so the module loader can use it without
 * linking the whole crypto implementation into standalone test drivers. */
static inline unsigned aphelion_native_module_flag(const char *name) {
    if (name != NULL && strcmp(name, "acsl") == 0) {
        return APHELION_NATIVE_MODULE_ACSL;
    }
    return 0u;
}

static inline bool aphelion_native_module_known(const char *name) {
    return aphelion_native_module_flag(name) != 0u;
}

static inline unsigned aphelion_native_module_flag_n(const char *name,
                                                     size_t length) {
    if (name != NULL && length == 4u && memcmp(name, "acsl", 4u) == 0) {
        return APHELION_NATIVE_MODULE_ACSL;
    }
    return 0u;
}

/* Registry lookup used by the semantic checker and compiler. */
unsigned aphelion_native_function_count(void);
const AphelionNativeFunction *aphelion_native_function_at(unsigned index);
int aphelion_native_find(const char *module, const char *name);

/* Runtime dispatch.  Validates argument types and arity again so a hand-made
 * .apb cannot bypass the front end. */
bool aphelion_native_call(unsigned index, const AphelionValue *arguments,
                          size_t argument_count, AphelionValue *result,
                          char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_NATIVE_H */
