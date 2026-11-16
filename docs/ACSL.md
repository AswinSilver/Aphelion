# ACSL — Aphelion Cryptography Security Library

ACSL is a compiled-in native module for Aphelion.  Programs import it with
`summon acsl` and call functions as members of the `acsl` name:

```agne
summon acsl

reveal(acsl.sha256_hex("abc"))
```

The module provides hashing, message authentication, authenticated encryption,
a cryptographically secure random generator, and password-based key derivation.
The algorithms come from OpenSSL's `libcrypto` and the pinned upstream Argon2
reference implementation; none of them are implemented inside Aphelion.

Supported platforms are **Windows x64** and **Linux x64**.  macOS is not a
supported target.

## Installation

ACSL is part of the normal Aphelion build, so there is nothing to install for
end users beyond the Aphelion package itself.

### Build requirements

- A C11 compiler and the platform C runtime.
- OpenSSL 3.x development headers and `libcrypto`.
- No separate Argon2 package: `third_party/argon2` contains the pinned upstream
  source (tag `20190702`) and is compiled into the binary.

Linux (Debian/Ubuntu):

```sh
sudo apt install build-essential libssl-dev
make build          # or: cmake -S . -B build && cmake --build build
```

Windows (MSYS2 UCRT64):

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-openssl
make build
```

A direct GCC build needs the vendored Argon2 include directory and `-lcrypto`:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -Iinclude -Ithird_party/argon2/include src/*.c -lcrypto -o aphelion
```

`OPENSSL_CFLAGS` and `OPENSSL_LIBS` override OpenSSL discovery in the
`Makefile`; CMake uses `find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)`
and honours `OpenSSL_ROOT_DIR`.

### Runtime dependencies

`libcrypto` is linked dynamically.  End users do not need a compiler or
development headers, but the OpenSSL runtime library must be available:

- **Linux x64**: `libcrypto.so.3`, normally installed by the distribution
  (`libssl3`/`openssl-libs`).
- **Windows x64**: `libcrypto-3-x64.dll`, shipped next to `aphelion.exe` in the
  release package.  `docs/RELEASE.md` describes the packaged layout.

If the library is missing, the operating system reports a load failure before
Aphelion starts; the program itself cannot report it.

## Import syntax

```agne
summon acsl        // unquoted native import (preferred)
summon "acsl"      // quoted form, accepted for consistency with file modules
```

`summon` must appear in the top-level preamble before any declaration or work.
Native imports are resolved by name and never touch the filesystem, so a
program that only imports `acsl` needs no `.agne` module files.  A call without
the import is a semantic error:

```text
example.agne:3:8: native module 'acsl' is not imported; add 'summon acsl'
```

## Types and byte conventions

Aphelion `text` values are byte strings, and every ACSL function treats them as
raw bytes:

- `text` parameters and results may contain any byte, including NUL and bytes
  that are not valid UTF-8.
- Hash, HMAC, ciphertext, key, and random functions return **raw bytes**.
- Every raw-byte result has a `_hex` (or `key_to_hex`/`key_to_base64`) helper
  for printable output.
- `whole` parameters are 64-bit signed integers; every function validates its
  own range.
- Functions returning `signal` are suitable for `when`/`assert`; they never fail
  at runtime.

Text equality and `length` are byte-exact, so comparing two raw digests with
`==` is correct.

## Function reference

### Hashing

| Function | Signature | Result |
| --- | --- | --- |
| `sha256` | `sha256(text) -> text` | 32 raw bytes |
| `sha256_hex` | `sha256_hex(text) -> text` | 64 lowercase hex characters |
| `sha512` | `sha512(text) -> text` | 64 raw bytes |
| `sha512_hex` | `sha512_hex(text) -> text` | 128 lowercase hex characters |
| `sha3_256` | `sha3_256(text) -> text` | 32 raw bytes (SHA-3) |
| `sha3_256_hex` | `sha3_256_hex(text) -> text` | 64 lowercase hex characters |
| `sha3_512` | `sha3_512(text) -> text` | 64 raw bytes (SHA-3) |
| `sha3_512_hex` | `sha3_512_hex(text) -> text` | 128 lowercase hex characters |

SHA-3 here is the FIPS 202 version, not the legacy Keccak padding.

```agne
summon acsl
reveal(acsl.sha256_hex("abc"))
// ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
```

### Message authentication (HMAC)

| Function | Signature | Result |
| --- | --- | --- |
| `hmac_sha256` | `hmac_sha256(text key, text message) -> text` | 32 raw bytes |
| `hmac_sha256_hex` | `hmac_sha256_hex(text key, text message) -> text` | 64 hex characters |
| `hmac_sha512` | `hmac_sha512(text key, text message) -> text` | 64 raw bytes |
| `hmac_sha512_hex` | `hmac_sha512_hex(text key, text message) -> text` | 128 hex characters |
| `hmac_sha256_verify` | `hmac_sha256_verify(text key, text message, text mac) -> signal` | constant-time verdict |
| `hmac_sha512_verify` | `hmac_sha512_verify(text key, text message, text mac) -> signal` | constant-time verdict |

`*_verify` accepts the expected MAC as either raw bytes or as lowercase/uppercase
hex.  A MAC of any other length is rejected without comparison.  The comparison
itself uses OpenSSL's constant-time `CRYPTO_memcmp`.

```agne
forge tag = acsl.hmac_sha256(key, "release manifest")
when acsl.hmac_sha256_verify(key, "release manifest", tag) {
    reveal("trusted")
} otherwise {
    reveal("rejected")
}
```

### Constant-time comparison

`constant_time_equal(text, text) -> signal`

Compares two byte strings in constant time with respect to their contents.
Different lengths return `dim` immediately; equal-length inputs are compared
with `CRYPTO_memcmp`.  Use this instead of `==` when comparing secrets.

### Text encodings

| Function | Signature | Behaviour |
| --- | --- | --- |
| `hex_encode` | `hex_encode(text) -> text` | Lowercase hex, two characters per byte |
| `hex_decode` | `hex_decode(text) -> text` | Accepts upper/lowercase; requires even length |
| `base64_encode` | `base64_encode(text) -> text` | Standard padded Base64 |
| `base64_decode` | `base64_decode(text) -> text` | Requires a multiple of four characters |

Invalid input is a runtime error (`odd length`, `non-hex character`,
`invalid character`, misplaced padding, and so on).  Neither decoder allocates
output for malformed input.

### Authenticated encryption

| Function | Signature | Nonce |
| --- | --- | --- |
| `encrypt` | `encrypt(text plaintext, text key) -> text` | fresh random 12-byte nonce |
| `decrypt` | `decrypt(text envelope, text key) -> text` | read from the envelope |
| `encrypt_aes256gcm` | `encrypt_aes256gcm(text, text) -> text` | fresh random nonce |
| `decrypt_aes256gcm` | `decrypt_aes256gcm(text, text) -> text` | AES-256-GCM only |
| `encrypt_chacha20poly1305` | `encrypt_chacha20poly1305(text, text) -> text` | fresh random nonce |
| `decrypt_chacha20poly1305` | `decrypt_chacha20poly1305(text, text) -> text` | ChaCha20-Poly1305 only |
| `encrypt_aes256gcm_with_nonce` | `encrypt_aes256gcm_with_nonce(text, text, text nonce) -> text` | caller-supplied 12 bytes |
| `encrypt_chacha20poly1305_with_nonce` | `encrypt_chacha20poly1305_with_nonce(text, text, text nonce) -> text` | caller-supplied 12 bytes |

Rules:

- The key must be exactly **32 bytes**.  There is no implicit hashing or
  padding of short keys.
- `encrypt` defaults to **AES-256-GCM**; there is no unauthenticated mode.
- The nonce must be exactly **12 bytes** for the `_with_nonce` variants.
- `encrypt`/`encrypt_aes256gcm`/`encrypt_chacha20poly1305` generate a fresh
  nonce with the platform CSPRNG on every call.  Never reuse a nonce with the
  same key; prefer the nonce-generating variants.
- `decrypt` reads the algorithm from the envelope.  The algorithm-specific
  decrypt functions reject an envelope produced by the other algorithm.
- Authentication failure (wrong key, modified nonce, tag, or ciphertext,
  truncated data) is a runtime error and **no plaintext is returned**.  A
  partially decrypted buffer is cleansed and freed before the error propagates.

```agne
summon acsl

forge key = acsl.generate_key(32)
forge sealed = acsl.encrypt("attack at dawn", key)
reveal(acsl.decrypt(sealed, key))   // attack at dawn
```

#### Envelope format

Ciphertext is a self-describing binary text value:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | ASCII magic `ACSL` |
| 4 | 1 | envelope version (`1`) |
| 5 | 1 | algorithm (`1` = AES-256-GCM, `2` = ChaCha20-Poly1305) |
| 6 | 1 | nonce length (`12`) |
| 7 | 1 | tag length (`16`) |
| 8 | 12 | nonce |
| 20 | 16 | authentication tag |
| 36 | n | ciphertext (same length as the plaintext) |

The minimum envelope is 36 bytes, so encrypting empty text produces exactly 36
bytes.  The envelope contains no key material and no length field beyond the
text length Aphelion already tracks.

### Randomness

| Function | Signature | Behaviour |
| --- | --- | --- |
| `random_bytes` | `random_bytes(whole) -> text` | `n` raw random bytes, 0..1048576 |
| `random_token` | `random_token(whole) -> text` | `n` lowercase hex characters |
| `random_int` | `random_int(whole low, whole high) -> whole` | unbiased value in `[low, high]` |
| `generate_key` | `generate_key(whole) -> text` | `n` raw key bytes, 1..1024 |

`random_bytes`, `random_token`, and `generate_key` use OpenSSL's
`RAND_priv_bytes`.  `random_int` rejects a full 64-bit range correctly and
rejection-samples to avoid modulo bias.  `low > high` is an error.

```agne
reveal(acsl.random_token(32))      // 32 hex characters
reveal(acsl.random_int(1, 6))      // 1..6
forge key = acsl.generate_key(32)  // AES-256 key
```

### Password-based key derivation

| Function | Signature | Notes |
| --- | --- | --- |
| `derive_key` | `derive_key(text password, text salt) -> text` | Argon2id, 32-byte key, salt ≥ 16 bytes |
| `argon2id` | `argon2id(text password, text salt, whole time, whole memory_kib, whole parallelism, whole out_length) -> text` | Full parameter control |
| `pbkdf2_sha256` | `pbkdf2_sha256(text password, text salt, whole iterations, whole out_length) -> text` | PBKDF2-HMAC-SHA256 |

Defaults and limits:

- `derive_key`: Argon2id with `time = 3`, `memory = 65536 KiB`, `parallelism = 1`,
  a 32-byte output, and a salt of at least 16 bytes.  This is the recommended
  high-level API.
- `argon2id`: `time` 1..64, `memory_kib` 8..1048576, `parallelism` 1..16,
  memory at least `8 * parallelism` KiB, salt 8..1024 bytes, output 1..1024
  bytes.  Salts shorter than 8 bytes are rejected by Argon2 itself.
- `pbkdf2_sha256`: `iterations` 1..1000000, salt 8..1024 bytes, output 1..1024
  bytes.

All KDFs write into a local buffer that is cleansed before it is released.

```agne
forge salt = acsl.random_bytes(16)
forge key = acsl.derive_key("correct horse battery staple", salt)
```

Salts must be unique per stored password and may be stored beside the derived
key.  Passwords, keys, and derived output are never included in diagnostics.

### Key helpers

| Function | Signature | Behaviour |
| --- | --- | --- |
| `key_from_hex` | `key_from_hex(text) -> text` | Decode hex key material |
| `key_to_hex` | `key_to_hex(text) -> text` | Encode key material as lowercase hex |
| `key_from_base64` | `key_from_base64(text) -> text` | Decode Base64 key material |
| `key_to_base64` | `key_to_base64(text) -> text` | Encode key material as Base64 |
| `validate_key` | `validate_key(text, whole) -> signal` | Length check only |

These are byte-encoding helpers with names that make key handling readable.
`validate_key` checks the byte length only; it does not assess entropy or
strength.

```agne
forge key = acsl.key_from_hex("000102...1f")
when acsl.validate_key(key, 32) { reveal("usable as an AES-256 key") }
```

## Limits

| Limit | Value |
| --- | --- |
| Hash/HMAC input | 64 MiB |
| Plaintext per encryption | 64 MiB |
| Random bytes / token characters | 1 048 576 |
| `generate_key` length | 1..1024 bytes |
| KDF output length | 1..1024 bytes |
| Native function arity | 8 arguments |
| Error message buffer | 256 bytes |

All lengths are validated before allocation, and size arithmetic is checked for
overflow.  Allocation failure produces a runtime error instead of undefined
behaviour.

## Errors

Front-end errors (unknown module/function, wrong arity, wrong type, member
access that is not called) are reported by `check` and `run` before execution
starts.  Runtime errors are reported as
`file:line:column: runtime error: message` and terminate the program with exit
status 1.  Representative messages:

| Situation | Message |
| --- | --- |
| Missing import | `native module 'acsl' is not imported; add 'summon acsl'` |
| Unknown function | `native module 'acsl' has no function 'x'` |
| Bad key length | `AES-256-GCM requires a 32-byte key` |
| Bad nonce length | `AES-256-GCM requires a 12-byte nonce` |
| Modified ciphertext | `authentication failed: ciphertext or tag was modified` |
| Truncated ciphertext | `ciphertext envelope is truncated` |
| Wrong algorithm | `ciphertext uses ChaCha20-Poly1305, not the requested algorithm` |
| Bad Argon2 parameter | `Argon2id memory cost must be between 8 and 1048576 KiB` |
| Short salt | `derive_key salt must be at least 16 bytes` |

Diagnostics never contain key bytes, passwords, derived keys, or plaintext.

## Security notes

- **No unauthenticated encryption.**  Only AEAD modes are exposed, and the tag
  is always verified before plaintext is returned.
- **Nonce handling is automatic** for `encrypt` and the algorithm-specific
  encrypt functions.  Use the `_with_nonce` variants only when replaying a
  published test vector or interoperating with another system that supplies
  nonces; never encrypt two messages with the same key and nonce.
- **Keys are not stored or logged.**  ACSL is a stateless library: it does not
  keep keys in globals, does not write secrets to disk, and does not include
  secrets in error text.
- **Memory cleansing.**  Temporary plaintext, key, tag, and nonce buffers are
  cleared with `OPENSSL_cleanse` before being freed.  Aphelion `text` values are
  reference counted and are not automatically cleansed; drop references as soon
  as a secret is no longer needed.
- **No associated data.**  The envelope has no AAD field.  Bind context by
  including it in the plaintext or by authenticating a separate HMAC over the
  context and ciphertext.
- **Constant-time comparison** is used for MAC verification.  `==` on text is
  not constant time.
- **Argon2 is single-threaded** in this build (`ARGON2_NO_THREADS`) to keep the
  runtime free of threading dependencies.  This affects speed, not the derived
  output.

## Example program

[`examples/acsl_demo.agne`](../examples/acsl_demo.agne) is executed by the test
suite:

```agne
// ACSL demo: hashing, authenticated encryption, integrity checks, and
// secure randomness.  Run it with: aphelion run examples/acsl_demo.agne
summon acsl

reveal("sha256: " + acsl.sha256_hex("aphelion"))

forge key = acsl.generate_key(32)
forge sealed = acsl.encrypt("secret message", key)
reveal("decrypted: " + acsl.decrypt(sealed, key))

forge tag = acsl.hmac_sha256(key, "release manifest")
when acsl.hmac_sha256_verify(key, "release manifest", tag) {
    reveal("authenticated: bright")
} otherwise {
    reveal("authenticated: dim")
}
when acsl.hmac_sha256_verify(key, "release manifest!", tag) {
    reveal("tamper accepted")
} otherwise {
    reveal("tamper rejected")
}

forge salt = acsl.random_bytes(16)
forge derived = acsl.derive_key("correct horse battery staple", salt)
reveal("derived key: " + acsl.key_to_hex(derived))

reveal("session token: " + acsl.random_token(32))
reveal(acsl.random_int(1, 6))
```

```text
aphelion run examples/acsl_demo.agne
```

## Testing

`tests/test_acsl.py` covers published vectors (SHA-2/SHA-3, RFC 4231 HMAC,
NIST AES-256-GCM, the Argon2id reference vector), cross-checks against Python's
`hashlib`/`hmac`, round trips for both AEAD ciphers, tamper and wrong-key
failures, malformed envelopes, key/nonce/KDF validation, randomness properties,
the documented demo, and a C-level driver
(`tests/support/acsl_checks.c`) that fails every allocation point inside a
native call and verifies that buffers are released and results stay hollow.
