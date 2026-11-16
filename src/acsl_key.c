#include "acsl_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#define ACSL_ARGON2_MIN_SALT 8u
#define ACSL_ARGON2_MAX_SALT 1024u
#define ACSL_PBKDF2_MIN_SALT 8u
#define ACSL_PBKDF2_MAX_SALT 1024u

static bool acsl_fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error != NULL && error_size != 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

bool acsl_argon2id(const unsigned char *password, size_t password_length,
                   const unsigned char *salt, size_t salt_length,
                   uint32_t time_cost, uint32_t memory_kib,
                   uint32_t parallelism, unsigned char *out, size_t out_length,
                   char *error, size_t error_size) {
    int result;

    if (out == NULL || out_length == 0u || out_length > ACSL_KDF_MAX_OUTPUT) {
        return acsl_fail(error, error_size,
                         "Argon2id output length must be between 1 and 1024 bytes");
    }
    if (time_cost < 1u || time_cost > 64u) {
        return acsl_fail(error, error_size,
                         "Argon2id time cost must be between 1 and 64");
    }
    if (memory_kib < 8u || memory_kib > 1048576u) {
        return acsl_fail(error, error_size,
                         "Argon2id memory cost must be between 8 and 1048576 KiB");
    }
    if (parallelism < 1u || parallelism > 16u) {
        return acsl_fail(error, error_size,
                         "Argon2id parallelism must be between 1 and 16");
    }
    if (memory_kib < parallelism * 8u) {
        return acsl_fail(error, error_size,
                         "Argon2id memory cost must be at least 8 KiB per lane");
    }
    if (salt == NULL || salt_length < ACSL_ARGON2_MIN_SALT ||
        salt_length > ACSL_ARGON2_MAX_SALT) {
        return acsl_fail(error, error_size,
                         "Argon2id salt must be between 8 and 1024 bytes");
    }
    if (password_length > ACSL_MAX_INPUT ||
        (password_length != 0u && password == NULL)) {
        return acsl_fail(error, error_size, "Argon2id password is too large");
    }
    if (password_length > (size_t)UINT32_MAX ||
        salt_length > (size_t)UINT32_MAX) {
        return acsl_fail(error, error_size, "Argon2id input is too large");
    }
    result = argon2id_hash_raw(time_cost, memory_kib, parallelism,
                               password, password_length,
                               salt, salt_length, out, out_length);
    if (result != ARGON2_OK) {
        OPENSSL_cleanse(out, out_length);
        return acsl_fail(error, error_size, "Argon2id failed: %s",
                         argon2_error_message(result));
    }
    return true;
}

bool acsl_pbkdf2_sha256(const unsigned char *password, size_t password_length,
                        const unsigned char *salt, size_t salt_length,
                        uint32_t iterations, unsigned char *out,
                        size_t out_length, char *error, size_t error_size) {
    if (out == NULL || out_length == 0u || out_length > ACSL_KDF_MAX_OUTPUT) {
        return acsl_fail(error, error_size,
                         "PBKDF2 output length must be between 1 and 1024 bytes");
    }
    if (iterations < 1u || iterations > 1000000u) {
        return acsl_fail(error, error_size,
                         "PBKDF2 iteration count must be between 1 and 1000000");
    }
    if (salt == NULL || salt_length < ACSL_PBKDF2_MIN_SALT ||
        salt_length > ACSL_PBKDF2_MAX_SALT) {
        return acsl_fail(error, error_size,
                         "PBKDF2 salt must be between 8 and 1024 bytes");
    }
    if (password_length > ACSL_MAX_INPUT ||
        (password_length != 0u && password == NULL)) {
        return acsl_fail(error, error_size, "PBKDF2 password is too large");
    }
    if (password_length > (size_t)INT_MAX || salt_length > (size_t)INT_MAX ||
        out_length > (size_t)INT_MAX || iterations > (uint32_t)INT_MAX) {
        return acsl_fail(error, error_size, "PBKDF2 input is too large");
    }
    ERR_clear_error();
    if (PKCS5_PBKDF2_HMAC((const char *)password, (int)password_length,
                          salt, (int)salt_length, (int)iterations,
                          EVP_sha256(), (int)out_length, out) != 1) {
        unsigned long code = ERR_get_error();
        OPENSSL_cleanse(out, out_length);
        if (code != 0u) {
            char detail[160];
            ERR_error_string_n(code, detail, sizeof(detail));
            ERR_clear_error();
            return acsl_fail(error, error_size, "PBKDF2 failed: %s", detail);
        }
        return acsl_fail(error, error_size, "PBKDF2 failed");
    }
    return true;
}
