/*
 * The upstream Argon2 reference implementation (tag 20190702, commit
 * 62358ba2123abd17fccf2a108a301d4b52c01a7c) is compiled as one translation
 * unit so the project's strict warning policy and single-source test builds
 * do not need a separate library.  The code is CC0/Apache-2.0 licensed; see
 * third_party/argon2/LICENSE.
 *
 * ARGON2_NO_THREADS keeps the runtime free of pthread/Windows thread
 * dependencies.  Lanes are processed sequentially, which is correct for the
 * reference algorithm and sufficient for the supported parameter range.
 */
/*
 * glibc declares explicit_bzero() only under _DEFAULT_SOURCE, while Argon2's
 * __GLIBC_PREREQ check still selects it in a strict -std=c11 build.  Declare
 * the documented glibc prototype here instead of patching the pinned upstream
 * sources; core.c's later _DEFAULT_SOURCE definition is then harmless.
 */
#if defined(__linux__)
#include <stddef.h>
extern void explicit_bzero(void *s, size_t n);
#endif

#define ARGON2_NO_THREADS 1

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#include "../third_party/argon2/src/argon2.c"
#include "../third_party/argon2/src/core.c"
#include "../third_party/argon2/src/encoding.c"
#include "../third_party/argon2/src/ref.c"
#include "../third_party/argon2/src/thread.c"
/* ref.c and blake2b.c both define a G macro for different purposes. */
#undef G
#include "../third_party/argon2/src/blake2/blake2b.c"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
