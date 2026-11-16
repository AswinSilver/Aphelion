#include "acsl_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#define ACSL_ENVELOPE_VERSION 1u
#define ACSL_ENVELOPE_MINIMUM (ACSL_ENVELOPE_HEADER + ACSL_NONCE_LENGTH + ACSL_TAG_LENGTH)

static bool acsl_fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error != NULL && error_size != 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
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

static const EVP_CIPHER *acsl_cipher_algorithm(uint8_t algorithm) {
    switch (algorithm) {
    case ACSL_ALGORITHM_AES_256_GCM:
        return EVP_aes_256_gcm();
    case ACSL_ALGORITHM_CHACHA20_POLY1305:
        return EVP_chacha20_poly1305();
    default:
        return NULL;
    }
}

static const char *acsl_algorithm_name(uint8_t algorithm) {
    switch (algorithm) {
    case ACSL_ALGORITHM_AES_256_GCM:
        return "AES-256-GCM";
    case ACSL_ALGORITHM_CHACHA20_POLY1305:
        return "ChaCha20-Poly1305";
    default:
        return "unknown";
    }
}

bool acsl_encrypt(uint8_t algorithm,
                  const unsigned char *plaintext, size_t plaintext_length,
                  const unsigned char *key, size_t key_length,
                  const unsigned char *nonce, size_t nonce_length,
                  unsigned char **out, size_t *out_length,
                  char *error, size_t error_size) {
    const EVP_CIPHER *cipher = acsl_cipher_algorithm(algorithm);
    EVP_CIPHER_CTX *context = NULL;
    unsigned char generated_nonce[ACSL_NONCE_LENGTH];
    unsigned char tag[ACSL_TAG_LENGTH];
    unsigned char *envelope = NULL;
    size_t total;
    int written = 0;
    int final_written = 0;
    bool ok = false;

    if (out == NULL || out_length == NULL) {
        return acsl_fail(error, error_size, "ciphertext output pointer is missing");
    }
    *out = NULL;
    *out_length = 0u;
    if (cipher == NULL) {
        return acsl_fail(error, error_size, "unsupported encryption algorithm");
    }
    if (key == NULL || key_length != ACSL_KEY_LENGTH) {
        return acsl_fail(error, error_size,
                         "%s requires a 32-byte key", acsl_algorithm_name(algorithm));
    }
    if (nonce_length != ACSL_NONCE_LENGTH) {
        return acsl_fail(error, error_size,
                         "%s requires a 12-byte nonce", acsl_algorithm_name(algorithm));
    }
    if (nonce == NULL) {
        if (!acsl_random_bytes(generated_nonce, sizeof(generated_nonce),
                               error, error_size)) {
            return false;
        }
        nonce = generated_nonce;
    }
    if (plaintext_length > ACSL_MAX_INPUT || plaintext_length > (size_t)INT_MAX) {
        return acsl_fail(error, error_size, "plaintext is too large");
    }
    if (plaintext_length != 0u && plaintext == NULL) {
        return acsl_fail(error, error_size, "plaintext buffer is missing");
    }
    if (plaintext_length > SIZE_MAX - ACSL_ENVELOPE_MINIMUM) {
        return acsl_fail(error, error_size, "ciphertext envelope is too large");
    }
    total = ACSL_ENVELOPE_MINIMUM + plaintext_length;
    envelope = (unsigned char *)malloc(total);
    if (envelope == NULL) {
        return acsl_fail(error, error_size, "out of memory creating ciphertext envelope");
    }
    context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        free(envelope);
        return acsl_fail(error, error_size, "out of memory creating cipher context");
    }
    ERR_clear_error();
    if (EVP_EncryptInit_ex(context, cipher, NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN,
                            (int)ACSL_NONCE_LENGTH, NULL) != 1 ||
        EVP_EncryptInit_ex(context, NULL, NULL, key, nonce) != 1) {
        ok = acsl_report_openssl(error, error_size, "cipher initialization");
        goto done;
    }
    if (plaintext_length != 0u) {
        if (EVP_EncryptUpdate(context, envelope + ACSL_ENVELOPE_MINIMUM,
                              &written, plaintext, (int)plaintext_length) != 1 ||
            written < 0 || (size_t)written != plaintext_length) {
            ok = acsl_report_openssl(error, error_size, "cipher encryption");
            goto done;
        }
    }
    if (EVP_EncryptFinal_ex(context,
                            envelope + ACSL_ENVELOPE_MINIMUM + (size_t)written,
                            &final_written) != 1 ||
        final_written != 0) {
        ok = acsl_report_openssl(error, error_size, "cipher finalization");
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG,
                            (int)ACSL_TAG_LENGTH, tag) != 1) {
        ok = acsl_report_openssl(error, error_size, "cipher tag retrieval");
        goto done;
    }
    memcpy(envelope, "ACSL", 4u);
    envelope[4] = (unsigned char)ACSL_ENVELOPE_VERSION;
    envelope[5] = algorithm;
    envelope[6] = (unsigned char)ACSL_NONCE_LENGTH;
    envelope[7] = (unsigned char)ACSL_TAG_LENGTH;
    memcpy(envelope + ACSL_ENVELOPE_HEADER, nonce, ACSL_NONCE_LENGTH);
    memcpy(envelope + ACSL_ENVELOPE_HEADER + ACSL_NONCE_LENGTH, tag,
           ACSL_TAG_LENGTH);
    *out = envelope;
    *out_length = total;
    envelope = NULL;
    ok = true;

done:
    OPENSSL_cleanse(tag, sizeof(tag));
    OPENSSL_cleanse(generated_nonce, sizeof(generated_nonce));
    EVP_CIPHER_CTX_free(context);
    free(envelope);
    return ok;
}

bool acsl_decrypt(uint8_t algorithm,
                  const unsigned char *envelope, size_t envelope_length,
                  const unsigned char *key, size_t key_length,
                  unsigned char **out, size_t *out_length,
                  char *error, size_t error_size) {
    const EVP_CIPHER *cipher;
    EVP_CIPHER_CTX *context = NULL;
    const unsigned char *nonce;
    const unsigned char *tag;
    const unsigned char *ciphertext;
    size_t ciphertext_length;
    unsigned char *plaintext = NULL;
    int written = 0;
    int final_written = 0;
    bool ok = false;

    if (out == NULL || out_length == NULL) {
        return acsl_fail(error, error_size, "plaintext output pointer is missing");
    }
    *out = NULL;
    *out_length = 0u;
    if (envelope == NULL || envelope_length < ACSL_ENVELOPE_MINIMUM) {
        return acsl_fail(error, error_size, "ciphertext envelope is truncated");
    }
    if (memcmp(envelope, "ACSL", 4u) != 0) {
        return acsl_fail(error, error_size, "ciphertext is not an ACSL envelope");
    }
    if (envelope[4] != ACSL_ENVELOPE_VERSION) {
        return acsl_fail(error, error_size, "unsupported ACSL envelope version");
    }
    if (envelope[6] != ACSL_NONCE_LENGTH || envelope[7] != ACSL_TAG_LENGTH) {
        return acsl_fail(error, error_size, "unsupported ACSL envelope nonce or tag length");
    }
    if (algorithm != 0u && envelope[5] != algorithm) {
        return acsl_fail(error, error_size,
                         "ciphertext uses %s, not the requested algorithm",
                         acsl_algorithm_name(envelope[5]));
    }
    cipher = acsl_cipher_algorithm(envelope[5]);
    if (cipher == NULL) {
        return acsl_fail(error, error_size, "unsupported encryption algorithm in envelope");
    }
    if (key == NULL || key_length != ACSL_KEY_LENGTH) {
        return acsl_fail(error, error_size,
                         "%s requires a 32-byte key",
                         acsl_algorithm_name(envelope[5]));
    }
    nonce = envelope + ACSL_ENVELOPE_HEADER;
    tag = nonce + ACSL_NONCE_LENGTH;
    ciphertext = tag + ACSL_TAG_LENGTH;
    ciphertext_length = envelope_length - ACSL_ENVELOPE_MINIMUM;
    if (ciphertext_length > (size_t)INT_MAX) {
        return acsl_fail(error, error_size, "ciphertext is too large");
    }
    plaintext = (unsigned char *)malloc(ciphertext_length != 0u
                                           ? ciphertext_length : 1u);
    if (plaintext == NULL) {
        return acsl_fail(error, error_size, "out of memory creating plaintext");
    }
    context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        free(plaintext);
        return acsl_fail(error, error_size, "out of memory creating cipher context");
    }
    ERR_clear_error();
    if (EVP_DecryptInit_ex(context, cipher, NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN,
                            (int)ACSL_NONCE_LENGTH, NULL) != 1 ||
        EVP_DecryptInit_ex(context, NULL, NULL, key, nonce) != 1) {
        ok = acsl_report_openssl(error, error_size, "cipher initialization");
        goto done;
    }
    if (ciphertext_length != 0u) {
        if (EVP_DecryptUpdate(context, plaintext, &written,
                              ciphertext, (int)ciphertext_length) != 1 ||
            written < 0 || (size_t)written != ciphertext_length) {
            ok = acsl_report_openssl(error, error_size, "cipher decryption");
            goto done;
        }
    }
    if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_TAG,
                            (int)ACSL_TAG_LENGTH, (void *)tag) != 1) {
        ok = acsl_report_openssl(error, error_size, "cipher tag setup");
        goto done;
    }
    if (EVP_DecryptFinal_ex(context, plaintext + (size_t)written,
                            &final_written) != 1 ||
        final_written != 0) {
        ERR_clear_error();
        ok = acsl_fail(error, error_size,
                       "authentication failed: ciphertext or tag was modified");
        goto done;
    }
    *out = plaintext;
    *out_length = ciphertext_length;
    plaintext = NULL;
    ok = true;

done:
    if (plaintext != NULL) {
        OPENSSL_cleanse(plaintext, ciphertext_length != 0u
                                      ? ciphertext_length : 1u);
    }
    EVP_CIPHER_CTX_free(context);
    free(plaintext);
    return ok;
}
