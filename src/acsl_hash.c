#include "acsl_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

static bool acsl_fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error != NULL && error_size != 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static void acsl_clear_openssl_error(void) {
    ERR_clear_error();
}

static bool acsl_report_openssl(char *error, size_t error_size,
                                const char *what) {
    unsigned long code = ERR_get_error();
    if (code == 0u) {
        return acsl_fail(error, error_size, "%s failed", what);
    }
    {
        char detail[160];
        ERR_error_string_n(code, detail, sizeof(detail));
        ERR_clear_error();
        return acsl_fail(error, error_size, "%s failed: %s", what, detail);
    }
}

static const EVP_MD *acsl_digest_algorithm(const char *algorithm) {
    if (algorithm == NULL) {
        return NULL;
    }
    if (strcmp(algorithm, "sha256") == 0) {
        return EVP_sha256();
    }
    if (strcmp(algorithm, "sha512") == 0) {
        return EVP_sha512();
    }
    if (strcmp(algorithm, "sha3-256") == 0) {
        return EVP_sha3_256();
    }
    if (strcmp(algorithm, "sha3-512") == 0) {
        return EVP_sha3_512();
    }
    return NULL;
}

bool acsl_hash(const char *algorithm, const unsigned char *data, size_t length,
               unsigned char *out, size_t out_length,
               char *error, size_t error_size) {
    const EVP_MD *md = acsl_digest_algorithm(algorithm);
    EVP_MD_CTX *context;
    unsigned int written = 0u;
    int expected;
    bool ok = false;

    if (md == NULL) {
        return acsl_fail(error, error_size, "unsupported hash algorithm");
    }
    if (out == NULL) {
        return acsl_fail(error, error_size, "hash output buffer is missing");
    }
    if (length > ACSL_MAX_INPUT) {
        return acsl_fail(error, error_size, "hash input is too large");
    }
    expected = EVP_MD_get_size(md);
    if (expected <= 0 || (size_t)expected != out_length) {
        return acsl_fail(error, error_size, "hash output length is invalid");
    }
    if (length != 0u && data == NULL) {
        return acsl_fail(error, error_size, "hash input buffer is missing");
    }
    context = EVP_MD_CTX_new();
    if (context == NULL) {
        return acsl_fail(error, error_size, "out of memory creating hash context");
    }
    acsl_clear_openssl_error();
    if (EVP_DigestInit_ex(context, md, NULL) != 1 ||
        EVP_DigestUpdate(context, data != NULL ? data : (const unsigned char *)"",
                         length) != 1 ||
        EVP_DigestFinal_ex(context, out, &written) != 1 ||
        (size_t)written != out_length) {
        ok = acsl_report_openssl(error, error_size, "hash computation");
    } else {
        ok = true;
    }
    EVP_MD_CTX_free(context);
    return ok;
}

bool acsl_hmac(const char *algorithm, const unsigned char *key, size_t key_length,
               const unsigned char *data, size_t data_length,
               unsigned char *out, size_t out_length,
               char *error, size_t error_size) {
    const EVP_MD *md = acsl_digest_algorithm(algorithm);
    unsigned int written = 0u;
    int expected;
    unsigned char *result;

    if (md == NULL) {
        return acsl_fail(error, error_size, "unsupported HMAC algorithm");
    }
    if (out == NULL) {
        return acsl_fail(error, error_size, "HMAC output buffer is missing");
    }
    if (key_length > ACSL_MAX_INPUT || data_length > ACSL_MAX_INPUT) {
        return acsl_fail(error, error_size, "HMAC input is too large");
    }
    if ((key_length != 0u && key == NULL) || (data_length != 0u && data == NULL)) {
        return acsl_fail(error, error_size, "HMAC input buffer is missing");
    }
    expected = EVP_MD_get_size(md);
    if (expected <= 0 || (size_t)expected != out_length) {
        return acsl_fail(error, error_size, "HMAC output length is invalid");
    }
    acsl_clear_openssl_error();
    result = HMAC(md, key, (int)key_length,
                  data != NULL ? data : (const unsigned char *)"", data_length,
                  out, &written);
    if (result == NULL || (size_t)written != out_length) {
        return acsl_report_openssl(error, error_size, "HMAC computation");
    }
    return true;
}

bool acsl_constant_time_equal(const unsigned char *left, size_t left_length,
                              const unsigned char *right, size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
    if (left_length == 0u) {
        return true;
    }
    if (left == NULL || right == NULL) {
        return false;
    }
    return CRYPTO_memcmp(left, right, left_length) == 0;
}

bool acsl_hex_encode(const unsigned char *data, size_t length,
                     char **out, char *error, size_t error_size) {
    static const char digits[] = "0123456789abcdef";
    char *text;
    size_t i;

    if (out == NULL) {
        return acsl_fail(error, error_size, "hex output pointer is missing");
    }
    *out = NULL;
    if (length > (SIZE_MAX - 1u) / 2u) {
        return acsl_fail(error, error_size, "hex output is too large");
    }
    text = (char *)malloc(length * 2u + 1u);
    if (text == NULL) {
        return acsl_fail(error, error_size, "out of memory encoding hex");
    }
    for (i = 0u; i < length; ++i) {
        text[i * 2u] = digits[data[i] >> 4u];
        text[i * 2u + 1u] = digits[data[i] & 0x0fu];
    }
    text[length * 2u] = '\0';
    *out = text;
    return true;
}

static int acsl_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

bool acsl_hex_decode(const char *text, size_t length,
                     unsigned char **out, size_t *out_length,
                     char *error, size_t error_size) {
    unsigned char *data;
    size_t i;

    if (out == NULL || out_length == NULL) {
        return acsl_fail(error, error_size, "hex output pointer is missing");
    }
    *out = NULL;
    *out_length = 0u;
    if ((length & 1u) != 0u) {
        return acsl_fail(error, error_size, "hex text must have an even length");
    }
    if (length / 2u > ACSL_MAX_INPUT) {
        return acsl_fail(error, error_size, "hex input is too large");
    }
    if (length != 0u && text == NULL) {
        return acsl_fail(error, error_size, "hex input is missing");
    }
    data = (unsigned char *)malloc(length / 2u != 0u ? length / 2u : 1u);
    if (data == NULL) {
        return acsl_fail(error, error_size, "out of memory decoding hex");
    }
    for (i = 0u; i < length; i += 2u) {
        int high = acsl_hex_value((unsigned char)text[i]);
        int low = acsl_hex_value((unsigned char)text[i + 1u]);
        if (high < 0 || low < 0) {
            free(data);
            return acsl_fail(error, error_size, "hex text contains a non-hex character");
        }
        data[i / 2u] = (unsigned char)((high << 4) | low);
    }
    *out = data;
    *out_length = length / 2u;
    return true;
}

bool acsl_base64_encode(const unsigned char *data, size_t length,
                        char **out, char *error, size_t error_size) {
    size_t output_length;
    char *text;

    if (out == NULL) {
        return acsl_fail(error, error_size, "base64 output pointer is missing");
    }
    *out = NULL;
    if (length > (SIZE_MAX - 2u) / 3u * 3u) {
        return acsl_fail(error, error_size, "base64 output is too large");
    }
    if (length != 0u && data == NULL) {
        return acsl_fail(error, error_size, "base64 input is missing");
    }
    if (length > (size_t)INT_MAX) {
        return acsl_fail(error, error_size, "base64 input is too large");
    }
    output_length = 4u * ((length + 2u) / 3u);
    if (output_length > SIZE_MAX - 1u) {
        return acsl_fail(error, error_size, "base64 output is too large");
    }
    text = (char *)malloc(output_length + 1u);
    if (text == NULL) {
        return acsl_fail(error, error_size, "out of memory encoding base64");
    }
    if (length == 0u) {
        text[0] = '\0';
    } else {
        int written = EVP_EncodeBlock((unsigned char *)text, data, (int)length);
        if (written < 0 || (size_t)written != output_length) {
            free(text);
            return acsl_fail(error, error_size, "base64 encoding failed");
        }
        text[output_length] = '\0';
    }
    *out = text;
    return true;
}

static bool acsl_base64_character(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}

bool acsl_base64_decode(const char *text, size_t length,
                        unsigned char **out, size_t *out_length,
                        char *error, size_t error_size) {
    unsigned char *data;
    size_t padding = 0u;
    size_t i;
    int written;

    if (out == NULL || out_length == NULL) {
        return acsl_fail(error, error_size, "base64 output pointer is missing");
    }
    *out = NULL;
    *out_length = 0u;
    if ((length % 4u) != 0u) {
        return acsl_fail(error, error_size, "base64 text length must be a multiple of four");
    }
    if (length != 0u && text == NULL) {
        return acsl_fail(error, error_size, "base64 input is missing");
    }
    if (length / 4u > ACSL_MAX_INPUT / 3u) {
        return acsl_fail(error, error_size, "base64 input is too large");
    }
    for (i = 0u; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '=') {
            if (i + 2u < length) {
                return acsl_fail(error, error_size, "base64 padding is misplaced");
            }
            padding += 1u;
        } else if (!acsl_base64_character(c)) {
            return acsl_fail(error, error_size, "base64 text contains an invalid character");
        } else if (padding != 0u) {
            return acsl_fail(error, error_size, "base64 padding is misplaced");
        }
    }
    if (padding > 2u) {
        return acsl_fail(error, error_size, "base64 padding is invalid");
    }
    data = (unsigned char *)malloc(length / 4u * 3u != 0u
                                       ? length / 4u * 3u : 1u);
    if (data == NULL) {
        return acsl_fail(error, error_size, "out of memory decoding base64");
    }
    if (length == 0u) {
        *out = data;
        *out_length = 0u;
        return true;
    }
    written = EVP_DecodeBlock(data, (const unsigned char *)text, (int)length);
    if (written < 0) {
        free(data);
        return acsl_fail(error, error_size, "base64 decoding failed");
    }
    if ((size_t)written < padding) {
        free(data);
        return acsl_fail(error, error_size, "base64 decoding failed");
    }
    *out = data;
    *out_length = (size_t)written - padding;
    return true;
}
