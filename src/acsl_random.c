#include "acsl_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/rand.h>

static bool acsl_fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error != NULL && error_size != 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

bool acsl_random_bytes(unsigned char *out, size_t length,
                       char *error, size_t error_size) {
    if (length > ACSL_MAX_RANDOM) {
        return acsl_fail(error, error_size, "random byte count is too large");
    }
    if (length == 0u) {
        return true;
    }
    if (out == NULL) {
        return acsl_fail(error, error_size, "random output buffer is missing");
    }
    if (length > (size_t)INT_MAX) {
        return acsl_fail(error, error_size, "random byte count is too large");
    }
    ERR_clear_error();
    if (RAND_priv_bytes(out, (int)length) != 1) {
        unsigned long code = ERR_get_error();
        if (code != 0u) {
            char detail[160];
            ERR_error_string_n(code, detail, sizeof(detail));
            ERR_clear_error();
            return acsl_fail(error, error_size,
                             "secure random generation failed: %s", detail);
        }
        return acsl_fail(error, error_size, "secure random generation failed");
    }
    return true;
}

bool acsl_random_int(int64_t low, int64_t high, int64_t *out,
                     char *error, size_t error_size) {
    uint64_t range;
    uint64_t limit;
    uint64_t value;

    if (out == NULL) {
        return acsl_fail(error, error_size, "random integer output is missing");
    }
    if (low > high) {
        return acsl_fail(error, error_size,
                         "random_int requires low to be less than or equal to high");
    }
    /* Unsigned arithmetic makes the full int64 range (range == 0) well defined. */
    range = (uint64_t)high - (uint64_t)low + 1u;
    for (;;) {
        if (!acsl_random_bytes((unsigned char *)&value, sizeof(value),
                               error, error_size)) {
            return false;
        }
        if (range == 0u) {
            break;
        }
        limit = UINT64_MAX - (UINT64_MAX % range);
        if (value < limit) {
            value %= range;
            break;
        }
    }
    *out = (int64_t)((uint64_t)low + value);
    return true;
}
