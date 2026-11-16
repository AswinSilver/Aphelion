#ifndef APHELION_ACSL_INTERNAL_H
#define APHELION_ACSL_INTERNAL_H

/*
 * Internal interfaces shared by the ACSL implementation groups.  None of
 * these symbols are part of the public Aphelion API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACSL_MAX_INPUT ((size_t)64u * 1024u * 1024u)
#define ACSL_MAX_RANDOM ((size_t)1024u * 1024u)
#define ACSL_MAX_KEY_LENGTH ((size_t)1024u)
#define ACSL_KDF_MAX_OUTPUT ((size_t)1024u)
#define ACSL_KEY_LENGTH 32u
#define ACSL_NONCE_LENGTH 12u
#define ACSL_TAG_LENGTH 16u
#define ACSL_ENVELOPE_HEADER 8u
#define ACSL_ERROR_SIZE 256u

#define ACSL_ALGORITHM_AES_256_GCM 1u
#define ACSL_ALGORITHM_CHACHA20_POLY1305 2u

/* Hashing, HMAC, constant-time comparison, and text encodings. */
bool acsl_hash(const char *algorithm, const unsigned char *data, size_t length,
               unsigned char *out, size_t out_length,
               char *error, size_t error_size);
bool acsl_hmac(const char *algorithm, const unsigned char *key, size_t key_length,
               const unsigned char *data, size_t data_length,
               unsigned char *out, size_t out_length,
               char *error, size_t error_size);
bool acsl_constant_time_equal(const unsigned char *left, size_t left_length,
                              const unsigned char *right, size_t right_length);
bool acsl_hex_encode(const unsigned char *data, size_t length,
                     char **out, char *error, size_t error_size);
bool acsl_hex_decode(const char *text, size_t length,
                     unsigned char **out, size_t *out_length,
                     char *error, size_t error_size);
bool acsl_base64_encode(const unsigned char *data, size_t length,
                        char **out, char *error, size_t error_size);
bool acsl_base64_decode(const char *text, size_t length,
                        unsigned char **out, size_t *out_length,
                        char *error, size_t error_size);

/* Cryptographically secure randomness. */
bool acsl_random_bytes(unsigned char *out, size_t length,
                       char *error, size_t error_size);
bool acsl_random_int(int64_t low, int64_t high, int64_t *out,
                     char *error, size_t error_size);

/* Authenticated encryption.  The envelope is self-describing. */
bool acsl_encrypt(uint8_t algorithm,
                  const unsigned char *plaintext, size_t plaintext_length,
                  const unsigned char *key, size_t key_length,
                  const unsigned char *nonce, size_t nonce_length,
                  unsigned char **out, size_t *out_length,
                  char *error, size_t error_size);
bool acsl_decrypt(uint8_t algorithm, /* 0 accepts any supported algorithm */
                  const unsigned char *envelope, size_t envelope_length,
                  const unsigned char *key, size_t key_length,
                  unsigned char **out, size_t *out_length,
                  char *error, size_t error_size);

/* Password-based key derivation. */
bool acsl_argon2id(const unsigned char *password, size_t password_length,
                   const unsigned char *salt, size_t salt_length,
                   uint32_t time_cost, uint32_t memory_kib,
                   uint32_t parallelism, unsigned char *out, size_t out_length,
                   char *error, size_t error_size);
bool acsl_pbkdf2_sha256(const unsigned char *password, size_t password_length,
                        const unsigned char *salt, size_t salt_length,
                        uint32_t iterations, unsigned char *out,
                        size_t out_length, char *error, size_t error_size);

#endif /* APHELION_ACSL_INTERNAL_H */
