#include "aphelion/native.h"

#include "acsl_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>

static bool acsl_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error != NULL && error_size != 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static bool acsl_text_arg(const AphelionValue *value,
                          const unsigned char **data, size_t *length) {
    if (value == NULL || value->type != APHELION_VALUE_TEXT ||
        value->as.text == NULL) {
        return false;
    }
    *data = (const unsigned char *)value->as.text->bytes;
    *length = value->as.text->length;
    return true;
}

static bool acsl_whole_arg(const AphelionValue *value, int64_t *number) {
    if (value == NULL || value->type != APHELION_VALUE_WHOLE) {
        return false;
    }
    *number = value->as.whole;
    return true;
}

static bool acsl_make_bytes(const unsigned char *data, size_t length,
                            AphelionValue *result, char *error,
                            size_t error_size) {
    *result = aphelion_value_text_n(
        (const char *)(data != NULL ? data : (const unsigned char *)""), length);
    if (result->type != APHELION_VALUE_TEXT) {
        return acsl_error(error, error_size,
                          "out of memory creating native text result");
    }
    return true;
}

static bool acsl_make_hex(const unsigned char *data, size_t length,
                          AphelionValue *result, char *error,
                          size_t error_size) {
    char *text = NULL;
    bool ok;
    if (!acsl_hex_encode(data, length, &text, error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes((const unsigned char *)text, strlen(text),
                         result, error, error_size);
    free(text);
    return ok;
}

static bool acsl_make_signal(bool value, AphelionValue *result) {
    *result = aphelion_value_signal(value);
    return true;
}

#define ACSL_HASH_WRAPPER(function_name, algorithm_name, digest_length)          \
    static bool function_name(const AphelionValue *arguments,                    \
                              size_t argument_count, AphelionValue *result,      \
                              char *error, size_t error_size) {                  \
        const unsigned char *data;                                               \
        size_t length;                                                           \
        unsigned char digest[digest_length];                                     \
        (void)argument_count;                                                    \
        if (!acsl_text_arg(&arguments[0], &data, &length)) {                     \
            return acsl_error(error, error_size,                                 \
                              #function_name " expects a text argument");        \
        }                                                                        \
        if (!acsl_hash(algorithm_name, data, length, digest, sizeof(digest),     \
                       error, error_size)) {                                     \
            return false;                                                        \
        }                                                                        \
        return acsl_make_bytes(digest, sizeof(digest), result, error,            \
                               error_size);                                      \
    }

#define ACSL_HASH_HEX_WRAPPER(function_name, algorithm_name, digest_length)      \
    static bool function_name(const AphelionValue *arguments,                    \
                              size_t argument_count, AphelionValue *result,      \
                              char *error, size_t error_size) {                  \
        const unsigned char *data;                                               \
        size_t length;                                                           \
        unsigned char digest[digest_length];                                     \
        (void)argument_count;                                                    \
        if (!acsl_text_arg(&arguments[0], &data, &length)) {                     \
            return acsl_error(error, error_size,                                 \
                              #function_name " expects a text argument");        \
        }                                                                        \
        if (!acsl_hash(algorithm_name, data, length, digest, sizeof(digest),     \
                       error, error_size)) {                                     \
            return false;                                                        \
        }                                                                        \
        return acsl_make_hex(digest, sizeof(digest), result, error, error_size); \
    }

#define ACSL_HMAC_WRAPPER(function_name, algorithm_name, digest_length)          \
    static bool function_name(const AphelionValue *arguments,                    \
                              size_t argument_count, AphelionValue *result,      \
                              char *error, size_t error_size) {                  \
        const unsigned char *key;                                                \
        const unsigned char *data;                                               \
        size_t key_length;                                                       \
        size_t data_length;                                                      \
        unsigned char mac[digest_length];                                        \
        (void)argument_count;                                                    \
        if (!acsl_text_arg(&arguments[0], &key, &key_length) ||                  \
            !acsl_text_arg(&arguments[1], &data, &data_length)) {                \
            return acsl_error(error, error_size,                                 \
                              #function_name " expects two text arguments");     \
        }                                                                        \
        if (!acsl_hmac(algorithm_name, key, key_length, data, data_length,       \
                       mac, sizeof(mac), error, error_size)) {                   \
            return false;                                                        \
        }                                                                        \
        return acsl_make_bytes(mac, sizeof(mac), result, error, error_size);     \
    }

#define ACSL_HMAC_HEX_WRAPPER(function_name, algorithm_name, digest_length)      \
    static bool function_name(const AphelionValue *arguments,                    \
                              size_t argument_count, AphelionValue *result,      \
                              char *error, size_t error_size) {                  \
        const unsigned char *key;                                                \
        const unsigned char *data;                                               \
        size_t key_length;                                                       \
        size_t data_length;                                                      \
        unsigned char mac[digest_length];                                        \
        (void)argument_count;                                                    \
        if (!acsl_text_arg(&arguments[0], &key, &key_length) ||                  \
            !acsl_text_arg(&arguments[1], &data, &data_length)) {                \
            return acsl_error(error, error_size,                                 \
                              #function_name " expects two text arguments");     \
        }                                                                        \
        if (!acsl_hmac(algorithm_name, key, key_length, data, data_length,       \
                       mac, sizeof(mac), error, error_size)) {                   \
            return false;                                                        \
        }                                                                        \
        return acsl_make_hex(mac, sizeof(mac), result, error, error_size);       \
    }

ACSL_HASH_WRAPPER(invoke_sha256, "sha256", 32)
ACSL_HASH_HEX_WRAPPER(invoke_sha256_hex, "sha256", 32)
ACSL_HASH_WRAPPER(invoke_sha512, "sha512", 64)
ACSL_HASH_HEX_WRAPPER(invoke_sha512_hex, "sha512", 64)
ACSL_HASH_WRAPPER(invoke_sha3_256, "sha3-256", 32)
ACSL_HASH_HEX_WRAPPER(invoke_sha3_256_hex, "sha3-256", 32)
ACSL_HASH_WRAPPER(invoke_sha3_512, "sha3-512", 64)
ACSL_HASH_HEX_WRAPPER(invoke_sha3_512_hex, "sha3-512", 64)

ACSL_HMAC_WRAPPER(invoke_hmac_sha256, "sha256", 32)
ACSL_HMAC_HEX_WRAPPER(invoke_hmac_sha256_hex, "sha256", 32)
ACSL_HMAC_WRAPPER(invoke_hmac_sha512, "sha512", 64)
ACSL_HMAC_HEX_WRAPPER(invoke_hmac_sha512_hex, "sha512", 64)

static bool acsl_verify_mac(const AphelionValue *arguments,
                            const char *algorithm, size_t digest_length,
                            AphelionValue *result, char *error,
                            size_t error_size) {
    const unsigned char *key;
    const unsigned char *data;
    const unsigned char *provided;
    size_t key_length;
    size_t data_length;
    size_t provided_length;
    unsigned char expected[64];
    unsigned char *decoded = NULL;
    size_t decoded_length = 0u;
    bool equal;
    bool ok;

    if (digest_length > sizeof(expected)) {
        return acsl_error(error, error_size, "internal MAC length is invalid");
    }
    if (!acsl_text_arg(&arguments[0], &key, &key_length) ||
        !acsl_text_arg(&arguments[1], &data, &data_length) ||
        !acsl_text_arg(&arguments[2], &provided, &provided_length)) {
        return acsl_error(error, error_size,
                          "HMAC verification expects three text arguments");
    }
    if (!acsl_hmac(algorithm, key, key_length, data, data_length,
                   expected, digest_length, error, error_size)) {
        return false;
    }
    if (provided_length == digest_length) {
        equal = acsl_constant_time_equal(expected, digest_length,
                                         provided, provided_length);
    } else if (provided_length == digest_length * 2u) {
        if (!acsl_hex_decode((const char *)provided, provided_length,
                             &decoded, &decoded_length, error, error_size)) {
            return false;
        }
        ok = decoded_length == digest_length;
        equal = ok && acsl_constant_time_equal(expected, digest_length,
                                               decoded, decoded_length);
        free(decoded);
    } else {
        equal = false;
    }
    OPENSSL_cleanse(expected, sizeof(expected));
    return acsl_make_signal(equal, result);
}

static bool invoke_hmac_sha256_verify(const AphelionValue *arguments,
                                      size_t argument_count,
                                      AphelionValue *result,
                                      char *error, size_t error_size) {
    (void)argument_count;
    return acsl_verify_mac(arguments, "sha256", 32, result, error, error_size);
}

static bool invoke_hmac_sha512_verify(const AphelionValue *arguments,
                                      size_t argument_count,
                                      AphelionValue *result,
                                      char *error, size_t error_size) {
    (void)argument_count;
    return acsl_verify_mac(arguments, "sha512", 64, result, error, error_size);
}

static bool invoke_constant_time_equal(const AphelionValue *arguments,
                                       size_t argument_count,
                                       AphelionValue *result,
                                       char *error, size_t error_size) {
    const unsigned char *left;
    const unsigned char *right;
    size_t left_length;
    size_t right_length;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &left, &left_length) ||
        !acsl_text_arg(&arguments[1], &right, &right_length)) {
        return acsl_error(error, error_size,
                          "constant_time_equal expects two text arguments");
    }
    return acsl_make_signal(
        acsl_constant_time_equal(left, left_length, right, right_length), result);
}

static bool invoke_hex_encode(const AphelionValue *arguments,
                              size_t argument_count, AphelionValue *result,
                              char *error, size_t error_size) {
    const unsigned char *data;
    size_t length;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &data, &length)) {
        return acsl_error(error, error_size,
                          "hex_encode expects a text argument");
    }
    return acsl_make_hex(data, length, result, error, error_size);
}

static bool invoke_hex_decode(const AphelionValue *arguments,
                              size_t argument_count, AphelionValue *result,
                              char *error, size_t error_size) {
    const unsigned char *data;
    size_t length;
    unsigned char *decoded = NULL;
    size_t decoded_length = 0u;
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &data, &length)) {
        return acsl_error(error, error_size,
                          "hex_decode expects a text argument");
    }
    if (!acsl_hex_decode((const char *)data, length, &decoded, &decoded_length,
                         error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes(decoded, decoded_length, result, error, error_size);
    /* Decoded text is frequently key material; wipe the temporary. */
    OPENSSL_cleanse(decoded, decoded_length != 0u ? decoded_length : 1u);
    free(decoded);
    return ok;
}

static bool invoke_base64_encode(const AphelionValue *arguments,
                                 size_t argument_count, AphelionValue *result,
                                 char *error, size_t error_size) {
    const unsigned char *data;
    size_t length;
    char *text = NULL;
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &data, &length)) {
        return acsl_error(error, error_size,
                          "base64_encode expects a text argument");
    }
    if (!acsl_base64_encode(data, length, &text, error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes((const unsigned char *)text, strlen(text),
                         result, error, error_size);
    free(text);
    return ok;
}

static bool invoke_base64_decode(const AphelionValue *arguments,
                                 size_t argument_count, AphelionValue *result,
                                 char *error, size_t error_size) {
    const unsigned char *data;
    size_t length;
    unsigned char *decoded = NULL;
    size_t decoded_length = 0u;
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &data, &length)) {
        return acsl_error(error, error_size,
                          "base64_decode expects a text argument");
    }
    if (!acsl_base64_decode((const char *)data, length, &decoded,
                            &decoded_length, error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes(decoded, decoded_length, result, error, error_size);
    OPENSSL_cleanse(decoded, decoded_length != 0u ? decoded_length : 1u);
    free(decoded);
    return ok;
}

static bool acsl_encrypt_value(uint8_t algorithm, bool explicit_nonce,
                               const AphelionValue *arguments,
                               AphelionValue *result, char *error,
                               size_t error_size) {
    const unsigned char *plaintext;
    const unsigned char *key;
    const unsigned char *nonce = NULL;
    size_t plaintext_length;
    size_t key_length;
    size_t nonce_length = ACSL_NONCE_LENGTH;
    unsigned char *envelope = NULL;
    size_t envelope_length = 0u;
    bool ok;

    if (!acsl_text_arg(&arguments[0], &plaintext, &plaintext_length) ||
        !acsl_text_arg(&arguments[1], &key, &key_length)) {
        return acsl_error(error, error_size,
                          "encryption expects text and a 32-byte key");
    }
    if (explicit_nonce &&
        !acsl_text_arg(&arguments[2], &nonce, &nonce_length)) {
        return acsl_error(error, error_size,
                          "encryption expects a 12-byte nonce");
    }
    if (!acsl_encrypt(algorithm, plaintext, plaintext_length, key, key_length,
                      nonce, nonce_length, &envelope, &envelope_length,
                      error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes(envelope, envelope_length, result, error, error_size);
    OPENSSL_cleanse(envelope, envelope_length);
    free(envelope);
    return ok;
}

static bool acsl_decrypt_value(uint8_t algorithm,
                               const AphelionValue *arguments,
                               AphelionValue *result, char *error,
                               size_t error_size) {
    const unsigned char *envelope;
    const unsigned char *key;
    size_t envelope_length;
    size_t key_length;
    unsigned char *plaintext = NULL;
    size_t plaintext_length = 0u;
    bool ok;

    if (!acsl_text_arg(&arguments[0], &envelope, &envelope_length) ||
        !acsl_text_arg(&arguments[1], &key, &key_length)) {
        return acsl_error(error, error_size,
                          "decryption expects ciphertext and a 32-byte key");
    }
    if (!acsl_decrypt(algorithm, envelope, envelope_length, key, key_length,
                      &plaintext, &plaintext_length, error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes(plaintext, plaintext_length, result, error, error_size);
    OPENSSL_cleanse(plaintext, plaintext_length != 0u ? plaintext_length : 1u);
    free(plaintext);
    return ok;
}

static bool invoke_encrypt(const AphelionValue *arguments,
                           size_t argument_count, AphelionValue *result,
                           char *error, size_t error_size) {
    (void)argument_count;
    return acsl_encrypt_value(ACSL_ALGORITHM_AES_256_GCM, false, arguments,
                              result, error, error_size);
}

static bool invoke_decrypt(const AphelionValue *arguments,
                           size_t argument_count, AphelionValue *result,
                           char *error, size_t error_size) {
    (void)argument_count;
    return acsl_decrypt_value(0u, arguments, result, error, error_size);
}

static bool invoke_encrypt_aes256gcm(const AphelionValue *arguments,
                                     size_t argument_count,
                                     AphelionValue *result,
                                     char *error, size_t error_size) {
    (void)argument_count;
    return acsl_encrypt_value(ACSL_ALGORITHM_AES_256_GCM, false, arguments,
                              result, error, error_size);
}

static bool invoke_decrypt_aes256gcm(const AphelionValue *arguments,
                                     size_t argument_count,
                                     AphelionValue *result,
                                     char *error, size_t error_size) {
    (void)argument_count;
    return acsl_decrypt_value(ACSL_ALGORITHM_AES_256_GCM, arguments, result,
                              error, error_size);
}

static bool invoke_encrypt_chacha20poly1305(const AphelionValue *arguments,
                                            size_t argument_count,
                                            AphelionValue *result,
                                            char *error, size_t error_size) {
    (void)argument_count;
    return acsl_encrypt_value(ACSL_ALGORITHM_CHACHA20_POLY1305, false,
                              arguments, result, error, error_size);
}

static bool invoke_decrypt_chacha20poly1305(const AphelionValue *arguments,
                                            size_t argument_count,
                                            AphelionValue *result,
                                            char *error, size_t error_size) {
    (void)argument_count;
    return acsl_decrypt_value(ACSL_ALGORITHM_CHACHA20_POLY1305, arguments,
                              result, error, error_size);
}

static bool invoke_encrypt_aes256gcm_with_nonce(const AphelionValue *arguments,
                                                size_t argument_count,
                                                AphelionValue *result,
                                                char *error,
                                                size_t error_size) {
    (void)argument_count;
    return acsl_encrypt_value(ACSL_ALGORITHM_AES_256_GCM, true, arguments,
                              result, error, error_size);
}

static bool invoke_encrypt_chacha20poly1305_with_nonce(
    const AphelionValue *arguments, size_t argument_count,
    AphelionValue *result, char *error, size_t error_size) {
    (void)argument_count;
    return acsl_encrypt_value(ACSL_ALGORITHM_CHACHA20_POLY1305, true,
                              arguments, result, error, error_size);
}

static bool acsl_random_count(const AphelionValue *value, size_t maximum,
                              size_t *count, char *error,
                              size_t error_size) {
    int64_t number;
    if (!acsl_whole_arg(value, &number)) {
        return acsl_error(error, error_size,
                          "random byte count must be a whole number");
    }
    if (number < 0 || (uint64_t)number > (uint64_t)maximum) {
        return acsl_error(error, error_size,
                          "random byte count must be between 0 and %zu", maximum);
    }
    *count = (size_t)number;
    return true;
}

static bool invoke_random_bytes(const AphelionValue *arguments,
                                size_t argument_count, AphelionValue *result,
                                char *error, size_t error_size) {
    size_t count = 0u;
    unsigned char *bytes;
    bool ok;
    (void)argument_count;
    if (!acsl_random_count(&arguments[0], ACSL_MAX_RANDOM, &count,
                           error, error_size)) {
        return false;
    }
    bytes = (unsigned char *)malloc(count != 0u ? count : 1u);
    if (bytes == NULL) {
        return acsl_error(error, error_size,
                          "out of memory creating random bytes");
    }
    if (!acsl_random_bytes(bytes, count, error, error_size)) {
        free(bytes);
        return false;
    }
    ok = acsl_make_bytes(bytes, count, result, error, error_size);
    OPENSSL_cleanse(bytes, count != 0u ? count : 1u);
    free(bytes);
    return ok;
}

static bool invoke_random_token(const AphelionValue *arguments,
                                size_t argument_count, AphelionValue *result,
                                char *error, size_t error_size) {
    size_t count = 0u;
    size_t byte_count;
    unsigned char *bytes;
    char *text = NULL;
    bool ok;
    (void)argument_count;
    if (!acsl_random_count(&arguments[0], ACSL_MAX_RANDOM * 2u, &count,
                           error, error_size)) {
        return false;
    }
    if (count == 0u) {
        return acsl_make_bytes((const unsigned char *)"", 0u, result,
                               error, error_size);
    }
    byte_count = (count + 1u) / 2u;
    bytes = (unsigned char *)malloc(byte_count);
    if (bytes == NULL) {
        return acsl_error(error, error_size,
                          "out of memory creating random token");
    }
    if (!acsl_random_bytes(bytes, byte_count, error, error_size)) {
        free(bytes);
        return false;
    }
    if (!acsl_hex_encode(bytes, byte_count, &text, error, error_size)) {
        OPENSSL_cleanse(bytes, byte_count);
        free(bytes);
        return false;
    }
    ok = acsl_make_bytes((const unsigned char *)text, count, result,
                         error, error_size);
    OPENSSL_cleanse(bytes, byte_count);
    free(bytes);
    /* The encoded token is a secret too; wipe the whole hex buffer. */
    OPENSSL_cleanse(text, byte_count * 2u);
    free(text);
    return ok;
}

static bool invoke_random_int(const AphelionValue *arguments,
                              size_t argument_count, AphelionValue *result,
                              char *error, size_t error_size) {
    int64_t low;
    int64_t high;
    int64_t value;
    (void)argument_count;
    if (!acsl_whole_arg(&arguments[0], &low) ||
        !acsl_whole_arg(&arguments[1], &high)) {
        return acsl_error(error, error_size,
                          "random_int expects two whole arguments");
    }
    if (!acsl_random_int(low, high, &value, error, error_size)) {
        return false;
    }
    *result = aphelion_value_whole(value);
    return true;
}

static bool invoke_generate_key(const AphelionValue *arguments,
                                size_t argument_count, AphelionValue *result,
                                char *error, size_t error_size) {
    size_t length = 0u;
    unsigned char *key;
    bool ok;
    (void)argument_count;
    if (!acsl_random_count(&arguments[0], ACSL_MAX_KEY_LENGTH, &length,
                           error, error_size)) {
        return false;
    }
    if (length == 0u) {
        return acsl_error(error, error_size,
                          "key length must be between 1 and 1024 bytes");
    }
    key = (unsigned char *)malloc(length);
    if (key == NULL) {
        return acsl_error(error, error_size, "out of memory generating key");
    }
    if (!acsl_random_bytes(key, length, error, error_size)) {
        free(key);
        return false;
    }
    ok = acsl_make_bytes(key, length, result, error, error_size);
    OPENSSL_cleanse(key, length);
    free(key);
    return ok;
}

static bool invoke_derive_key(const AphelionValue *arguments,
                              size_t argument_count, AphelionValue *result,
                              char *error, size_t error_size) {
    const unsigned char *password;
    const unsigned char *salt;
    size_t password_length;
    size_t salt_length;
    unsigned char key[ACSL_KEY_LENGTH];
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &password, &password_length) ||
        !acsl_text_arg(&arguments[1], &salt, &salt_length)) {
        return acsl_error(error, error_size,
                          "derive_key expects password and salt text");
    }
    if (salt_length < 16u) {
        return acsl_error(error, error_size,
                          "derive_key salt must be at least 16 bytes");
    }
    if (!acsl_argon2id(password, password_length, salt, salt_length,
                       3u, 65536u, 1u, key, sizeof(key), error, error_size)) {
        return false;
    }
    ok = acsl_make_bytes(key, sizeof(key), result, error, error_size);
    OPENSSL_cleanse(key, sizeof(key));
    return ok;
}

static bool invoke_argon2id(const AphelionValue *arguments,
                            size_t argument_count, AphelionValue *result,
                            char *error, size_t error_size) {
    const unsigned char *password;
    const unsigned char *salt;
    size_t password_length;
    size_t salt_length;
    int64_t time_cost;
    int64_t memory_kib;
    int64_t parallelism;
    int64_t key_length;
    unsigned char *key;
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &password, &password_length) ||
        !acsl_text_arg(&arguments[1], &salt, &salt_length) ||
        !acsl_whole_arg(&arguments[2], &time_cost) ||
        !acsl_whole_arg(&arguments[3], &memory_kib) ||
        !acsl_whole_arg(&arguments[4], &parallelism) ||
        !acsl_whole_arg(&arguments[5], &key_length)) {
        return acsl_error(error, error_size,
                          "argon2id expects password, salt, and four whole parameters");
    }
    if (time_cost < 0 || time_cost > (int64_t)UINT32_MAX ||
        memory_kib < 0 || memory_kib > (int64_t)UINT32_MAX ||
        parallelism < 0 || parallelism > (int64_t)UINT32_MAX ||
        key_length < 1 || key_length > (int64_t)ACSL_KDF_MAX_OUTPUT) {
        return acsl_error(error, error_size,
                          "argon2id parameters are out of range");
    }
    key = (unsigned char *)malloc((size_t)key_length);
    if (key == NULL) {
        return acsl_error(error, error_size, "out of memory deriving key");
    }
    if (!acsl_argon2id(password, password_length, salt, salt_length,
                       (uint32_t)time_cost, (uint32_t)memory_kib,
                       (uint32_t)parallelism, key, (size_t)key_length,
                       error, error_size)) {
        free(key);
        return false;
    }
    ok = acsl_make_bytes(key, (size_t)key_length, result, error, error_size);
    OPENSSL_cleanse(key, (size_t)key_length);
    free(key);
    return ok;
}

static bool invoke_pbkdf2_sha256(const AphelionValue *arguments,
                                 size_t argument_count, AphelionValue *result,
                                 char *error, size_t error_size) {
    const unsigned char *password;
    const unsigned char *salt;
    size_t password_length;
    size_t salt_length;
    int64_t iterations;
    int64_t key_length;
    unsigned char *key;
    bool ok;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &password, &password_length) ||
        !acsl_text_arg(&arguments[1], &salt, &salt_length) ||
        !acsl_whole_arg(&arguments[2], &iterations) ||
        !acsl_whole_arg(&arguments[3], &key_length)) {
        return acsl_error(error, error_size,
                          "pbkdf2_sha256 expects password, salt, iterations, and key length");
    }
    if (iterations < 0 || iterations > (int64_t)UINT32_MAX ||
        key_length < 1 || key_length > (int64_t)ACSL_KDF_MAX_OUTPUT) {
        return acsl_error(error, error_size,
                          "pbkdf2_sha256 parameters are out of range");
    }
    key = (unsigned char *)malloc((size_t)key_length);
    if (key == NULL) {
        return acsl_error(error, error_size, "out of memory deriving key");
    }
    if (!acsl_pbkdf2_sha256(password, password_length, salt, salt_length,
                            (uint32_t)iterations, key, (size_t)key_length,
                            error, error_size)) {
        free(key);
        return false;
    }
    ok = acsl_make_bytes(key, (size_t)key_length, result, error, error_size);
    OPENSSL_cleanse(key, (size_t)key_length);
    free(key);
    return ok;
}

static bool invoke_key_from_hex(const AphelionValue *arguments,
                                size_t argument_count, AphelionValue *result,
                                char *error, size_t error_size) {
    return invoke_hex_decode(arguments, argument_count, result, error, error_size);
}

static bool invoke_key_to_hex(const AphelionValue *arguments,
                              size_t argument_count, AphelionValue *result,
                              char *error, size_t error_size) {
    return invoke_hex_encode(arguments, argument_count, result, error, error_size);
}

static bool invoke_key_from_base64(const AphelionValue *arguments,
                                   size_t argument_count,
                                   AphelionValue *result,
                                   char *error, size_t error_size) {
    return invoke_base64_decode(arguments, argument_count, result, error, error_size);
}

static bool invoke_key_to_base64(const AphelionValue *arguments,
                                 size_t argument_count,
                                 AphelionValue *result,
                                 char *error, size_t error_size) {
    return invoke_base64_encode(arguments, argument_count, result, error, error_size);
}

static bool invoke_validate_key(const AphelionValue *arguments,
                                size_t argument_count, AphelionValue *result,
                                char *error, size_t error_size) {
    const unsigned char *key;
    size_t key_length;
    int64_t expected;
    (void)argument_count;
    if (!acsl_text_arg(&arguments[0], &key, &key_length) ||
        !acsl_whole_arg(&arguments[1], &expected)) {
        return acsl_error(error, error_size,
                          "validate_key expects a key and an expected length");
    }
    (void)key;
    if (expected < 1 || expected > (int64_t)ACSL_MAX_KEY_LENGTH) {
        return acsl_error(error, error_size,
                          "expected key length must be between 1 and 1024 bytes");
    }
    return acsl_make_signal(key_length == (size_t)expected, result);
}

static const AphelionNativeFunction acsl_functions[] = {
    {"acsl", "sha256", "sha256(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha256},
    {"acsl", "sha256_hex", "sha256_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha256_hex},
    {"acsl", "sha512", "sha512(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha512},
    {"acsl", "sha512_hex", "sha512_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha512_hex},
    {"acsl", "sha3_256", "sha3_256(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha3_256},
    {"acsl", "sha3_256_hex", "sha3_256_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha3_256_hex},
    {"acsl", "sha3_512", "sha3_512(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha3_512},
    {"acsl", "sha3_512_hex", "sha3_512_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_sha3_512_hex},
    {"acsl", "hmac_sha256", "hmac_sha256(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_hmac_sha256},
    {"acsl", "hmac_sha256_hex", "hmac_sha256_hex(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_hmac_sha256_hex},
    {"acsl", "hmac_sha512", "hmac_sha512(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_hmac_sha512},
    {"acsl", "hmac_sha512_hex", "hmac_sha512_hex(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_hmac_sha512_hex},
    {"acsl", "hmac_sha256_verify", "hmac_sha256_verify(text, text, text) -> signal", 3u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_TEXT}, TYPE_SIGNAL, invoke_hmac_sha256_verify},
    {"acsl", "hmac_sha512_verify", "hmac_sha512_verify(text, text, text) -> signal", 3u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_TEXT}, TYPE_SIGNAL, invoke_hmac_sha512_verify},
    {"acsl", "constant_time_equal", "constant_time_equal(text, text) -> signal", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_SIGNAL,
     invoke_constant_time_equal},
    {"acsl", "hex_encode", "hex_encode(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_hex_encode},
    {"acsl", "hex_decode", "hex_decode(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_hex_decode},
    {"acsl", "base64_encode", "base64_encode(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_base64_encode},
    {"acsl", "base64_decode", "base64_decode(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_base64_decode},
    {"acsl", "encrypt", "encrypt(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_encrypt},
    {"acsl", "decrypt", "decrypt(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_decrypt},
    {"acsl", "encrypt_aes256gcm", "encrypt_aes256gcm(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_encrypt_aes256gcm},
    {"acsl", "decrypt_aes256gcm", "decrypt_aes256gcm(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_decrypt_aes256gcm},
    {"acsl", "encrypt_chacha20poly1305",
     "encrypt_chacha20poly1305(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_encrypt_chacha20poly1305},
    {"acsl", "decrypt_chacha20poly1305",
     "decrypt_chacha20poly1305(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_decrypt_chacha20poly1305},
    {"acsl", "encrypt_aes256gcm_with_nonce",
     "encrypt_aes256gcm_with_nonce(text, text, text) -> text", 3u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_encrypt_aes256gcm_with_nonce},
    {"acsl", "encrypt_chacha20poly1305_with_nonce",
     "encrypt_chacha20poly1305_with_nonce(text, text, text) -> text", 3u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_encrypt_chacha20poly1305_with_nonce},
    {"acsl", "random_bytes", "random_bytes(whole) -> text", 1u,
     {APHELION_NATIVE_PARAM_WHOLE}, TYPE_TEXT, invoke_random_bytes},
    {"acsl", "random_token", "random_token(whole) -> text", 1u,
     {APHELION_NATIVE_PARAM_WHOLE}, TYPE_TEXT, invoke_random_token},
    {"acsl", "random_int", "random_int(whole, whole) -> whole", 2u,
     {APHELION_NATIVE_PARAM_WHOLE, APHELION_NATIVE_PARAM_WHOLE}, TYPE_WHOLE,
     invoke_random_int},
    {"acsl", "generate_key", "generate_key(whole) -> text", 1u,
     {APHELION_NATIVE_PARAM_WHOLE}, TYPE_TEXT, invoke_generate_key},
    {"acsl", "derive_key", "derive_key(text, text) -> text", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT,
     invoke_derive_key},
    {"acsl", "argon2id",
     "argon2id(text, text, whole, whole, whole, whole) -> text", 6u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_WHOLE, APHELION_NATIVE_PARAM_WHOLE,
      APHELION_NATIVE_PARAM_WHOLE, APHELION_NATIVE_PARAM_WHOLE}, TYPE_TEXT,
     invoke_argon2id},
    {"acsl", "pbkdf2_sha256",
     "pbkdf2_sha256(text, text, whole, whole) -> text", 4u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_TEXT,
      APHELION_NATIVE_PARAM_WHOLE, APHELION_NATIVE_PARAM_WHOLE}, TYPE_TEXT,
     invoke_pbkdf2_sha256},
    {"acsl", "key_from_hex", "key_from_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_key_from_hex},
    {"acsl", "key_to_hex", "key_to_hex(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_key_to_hex},
    {"acsl", "key_from_base64", "key_from_base64(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_key_from_base64},
    {"acsl", "key_to_base64", "key_to_base64(text) -> text", 1u,
     {APHELION_NATIVE_PARAM_TEXT}, TYPE_TEXT, invoke_key_to_base64},
    {"acsl", "validate_key", "validate_key(text, whole) -> signal", 2u,
     {APHELION_NATIVE_PARAM_TEXT, APHELION_NATIVE_PARAM_WHOLE}, TYPE_SIGNAL,
     invoke_validate_key}
};

unsigned aphelion_native_function_count(void) {
    return (unsigned)(sizeof(acsl_functions) / sizeof(acsl_functions[0]));
}

const AphelionNativeFunction *aphelion_native_function_at(unsigned index) {
    if (index >= aphelion_native_function_count()) {
        return NULL;
    }
    return &acsl_functions[index];
}

int aphelion_native_find(const char *module, const char *name) {
    unsigned i;
    if (module == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < aphelion_native_function_count(); ++i) {
        if (strcmp(acsl_functions[i].module, module) == 0 &&
            strcmp(acsl_functions[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool acsl_runtime_type_matches(AphelionNativeParam expected,
                                      const AphelionValue *value) {
    if (value == NULL) {
        return false;
    }
    switch (expected) {
    case APHELION_NATIVE_PARAM_TEXT:
        return value->type == APHELION_VALUE_TEXT && value->as.text != NULL;
    case APHELION_NATIVE_PARAM_WHOLE:
        return value->type == APHELION_VALUE_WHOLE;
    case APHELION_NATIVE_PARAM_SIGNAL:
        return value->type == APHELION_VALUE_SIGNAL;
    case APHELION_NATIVE_PARAM_ANY:
        return value->type != APHELION_VALUE_UNINITIALIZED;
    default:
        return false;
    }
}

static const char *acsl_native_type_name(AphelionNativeParam expected) {
    switch (expected) {
    case APHELION_NATIVE_PARAM_TEXT: return "text";
    case APHELION_NATIVE_PARAM_WHOLE: return "whole";
    case APHELION_NATIVE_PARAM_SIGNAL: return "signal";
    default: return "value";
    }
}

bool aphelion_native_call(unsigned index, const AphelionValue *arguments,
                          size_t argument_count, AphelionValue *result,
                          char *error, size_t error_size) {
    const AphelionNativeFunction *function;
    AphelionValue produced;
    size_t i;

    if (result == NULL) {
        return acsl_error(error, error_size, "native result pointer is missing");
    }
    *result = aphelion_value_hollow();
    function = aphelion_native_function_at(index);
    if (function == NULL) {
        return acsl_error(error, error_size, "native function index is invalid");
    }
    if (argument_count != function->arity) {
        return acsl_error(error, error_size,
                          "%s expects %u argument%s, got %zu",
                          function->signature, function->arity,
                          function->arity == 1u ? "" : "s", argument_count);
    }
    for (i = 0u; i < argument_count; ++i) {
        if (!acsl_runtime_type_matches(function->parameters[i], &arguments[i])) {
            return acsl_error(error, error_size,
                              "argument %zu to %s must be %s",
                              i + 1u, function->signature,
                              acsl_native_type_name(function->parameters[i]));
        }
    }
    produced = aphelion_value_hollow();
    if (!function->invoke(arguments, argument_count, &produced,
                          error, error_size)) {
        aphelion_value_destroy(&produced);
        if (error != NULL && error_size != 0u && error[0] == '\0') {
            (void)snprintf(error, error_size, "%s failed", function->signature);
        }
        return false;
    }
    *result = produced;
    return true;
}
