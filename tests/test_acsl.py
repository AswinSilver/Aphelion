"""End-to-end tests for the ACSL native cryptography module.

The tests drive the real compiler and VM through `.agne` programs.  Expected
digests and MACs come from Python's own hashlib/hmac implementations or from
published test vectors, so a regression in the C code cannot pass unnoticed.
"""

import base64
import hashlib
import hmac as hmaclib
import subprocess

import pytest

from conftest import (ROOT, compiler_command, crypto_include_flags, crypto_libraries,
                      math_libraries, native_executable_name,
                      test_cflags as extra_cflags)


PREAMBLE = "summon acsl\n"


def write_program(tmp_path, body, name="acsl.agne"):
    path = tmp_path / name
    path.write_text(PREAMBLE + body, encoding="utf-8")
    return path


def run_source(executable, tmp_path, body, name="acsl.agne", command="run"):
    path = write_program(tmp_path, body, name)
    return subprocess.run(
        [str(executable), command, str(path)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )


def lines(result):
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()


def single(result):
    values = lines(result)
    assert len(values) == 1, result.stdout
    return values[0]


def text_literal(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '"' + escaped + '"'


def hex_literal(data):
    return text_literal(data.hex())


# --------------------------------------------------------------------------
# Import syntax and front-end diagnostics
# --------------------------------------------------------------------------

def test_unquoted_and_quoted_summon_imports_both_work(aphelion_executable, tmp_path):
    unquoted = tmp_path / "unquoted.agne"
    unquoted.write_text(
        'summon acsl\nreveal(acsl.sha256_hex("abc"))\n', encoding="utf-8")
    quoted = tmp_path / "quoted.agne"
    quoted.write_text(
        'summon "acsl"\nreveal(acsl.sha256_hex("abc"))\n', encoding="utf-8")
    expected = hashlib.sha256(b"abc").hexdigest()
    for path in (unquoted, quoted):
        result = subprocess.run([str(aphelion_executable), "run", str(path)],
                                cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", timeout=30)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == expected


def test_native_member_requires_the_summon_import(aphelion_executable, tmp_path):
    path = tmp_path / "missing.agne"
    path.write_text('reveal(acsl.sha256_hex("abc"))\n', encoding="utf-8")
    result = subprocess.run([str(aphelion_executable), "check", str(path)],
                            cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", timeout=30)
    assert result.returncode == 1
    assert "not imported" in result.stderr
    assert "summon acsl" in result.stderr


@pytest.mark.parametrize(("body", "message"), [
    ('reveal(acsl.no_such_function("abc"))\n', "has no function"),
    ('reveal(acsl.sha256_hex)\n', "must be called"),
    ('forge x = acsl.sha256_hex("a", "b")\n', "expects 1 argument"),
    ('forge x = acsl.sha256_hex(12)\n', "expected text"),
    ('forge x = acsl.random_int("a", 2)\n', "expected whole"),
])
def test_native_call_diagnostics(aphelion_executable, tmp_path, body, message):
    result = run_source(aphelion_executable, tmp_path, body, command="check")
    assert result.returncode == 1
    assert message in result.stderr


def test_member_access_on_non_native_name_is_rejected(aphelion_executable, tmp_path):
    result = run_source(aphelion_executable, tmp_path,
                        'forge thing = 1\nreveal(thing.sha256_hex("a"))\n',
                        command="check")
    assert result.returncode == 1
    assert "native module" in result.stderr


# --------------------------------------------------------------------------
# Hashing
# --------------------------------------------------------------------------

HASH_VECTORS = [
    ("sha256", hashlib.sha256),
    ("sha512", hashlib.sha512),
    ("sha3_256", hashlib.sha3_256),
    ("sha3_512", hashlib.sha3_512),
]


@pytest.mark.parametrize(("name", "reference"), HASH_VECTORS)
@pytest.mark.parametrize("message", ["", "abc", "The quick brown fox"])
def test_hash_hex_matches_hashlib(aphelion_executable, tmp_path, name, reference, message):
    result = run_source(
        aphelion_executable, tmp_path,
        f"reveal(acsl.{name}_hex({text_literal(message)}))\n")
    assert single(result) == reference(message.encode()).hexdigest()


@pytest.mark.parametrize(("name", "reference"), HASH_VECTORS)
def test_hash_returns_raw_bytes_of_the_documented_length(aphelion_executable, tmp_path,
                                                          name, reference):
    expected = reference(b"abc").digest()
    result = run_source(
        aphelion_executable, tmp_path,
        f"reveal(length(acsl.{name}(\"abc\")))\n"
        f"reveal(acsl.hex_encode(acsl.{name}(\"abc\")))\n")
    values = lines(result)
    assert values == [str(len(expected)), expected.hex()]


def test_sha256_known_vector(aphelion_executable, tmp_path):
    result = run_source(aphelion_executable, tmp_path,
                        'reveal(acsl.sha256_hex("abc"))\n')
    assert single(result) == \
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


# --------------------------------------------------------------------------
# HMAC and constant-time comparison
# --------------------------------------------------------------------------

def test_hmac_sha256_rfc4231_case_two(aphelion_executable, tmp_path):
    result = run_source(
        aphelion_executable, tmp_path,
        'reveal(acsl.hmac_sha256_hex("Jefe", "what do ya want for nothing?"))\n')
    assert single(result) == \
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"


@pytest.mark.parametrize(("name", "reference"), [
    ("hmac_sha256", hashlib.sha256),
    ("hmac_sha512", hashlib.sha512),
])
@pytest.mark.parametrize(("key", "message"), [
    ("key", "message"),
    ("", ""),
    ("a" * 200, "long key exercises the block-size reduction path"),
])
def test_hmac_matches_python(aphelion_executable, tmp_path, name, reference,
                             key, message):
    expected = hmaclib.new(key.encode(), message.encode(), reference).hexdigest()
    result = run_source(
        aphelion_executable, tmp_path,
        f"reveal(acsl.{name}_hex({text_literal(key)}, {text_literal(message)}))\n")
    assert single(result) == expected


def test_hmac_verify_accepts_raw_and_hex_and_rejects_tampering(aphelion_executable,
                                                               tmp_path):
    body = (
        'forge mac = acsl.hmac_sha256("Jefe", "what do ya want for nothing?")\n'
        'when acsl.hmac_sha256_verify("Jefe", "what do ya want for nothing?", mac)'
        ' { reveal("raw") } otherwise { reveal("bad-raw") }\n'
        'when acsl.hmac_sha256_verify("Jefe", "what do ya want for nothing?",'
        ' acsl.hex_encode(mac)) { reveal("hex") } otherwise { reveal("bad-hex") }\n'
        'when acsl.hmac_sha256_verify("Jefe", "what do ya want for nothing?x", mac)'
        ' { reveal("tampered") } otherwise { reveal("rejected") }\n'
        'when acsl.hmac_sha256_verify("Jefe", "what do ya want for nothing?",'
        ' "00") { reveal("short") } otherwise { reveal("length-rejected") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["raw", "hex", "rejected", "length-rejected"]


def test_hmac_verify_rejects_invalid_hex(aphelion_executable, tmp_path):
    invalid = "z" * 64
    body = (
        'forge mac = acsl.hmac_sha256("k", "m")\n'
        f'reveal(acsl.hmac_sha256_verify("k", "m", "{invalid}"))\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert result.returncode == 1
    assert "non-hex character" in result.stderr


def test_constant_time_equal_compares_text_exactly(aphelion_executable, tmp_path):
    body = (
        'when acsl.constant_time_equal("abc", "abc") { reveal("same") }'
        ' otherwise { reveal("different") }\n'
        'when acsl.constant_time_equal("abc", "abd") { reveal("same") }'
        ' otherwise { reveal("different") }\n'
        'when acsl.constant_time_equal("", "") { reveal("same") }'
        ' otherwise { reveal("different") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["same", "different", "same"]


# --------------------------------------------------------------------------
# Text encodings
# --------------------------------------------------------------------------

@pytest.mark.parametrize("payload", [b"", b"a", b"ab", b"abc", bytes(range(256))])
def test_hex_and_base64_round_trip(aphelion_executable, tmp_path, payload):
    body = (
        f'forge raw = acsl.hex_decode({hex_literal(payload)})\n'
        'reveal(acsl.hex_encode(raw))\n'
        'reveal(acsl.base64_encode(raw))\n'
        'forge decoded = acsl.base64_decode(acsl.base64_encode(raw))\n'
        'when acsl.constant_time_equal(decoded, raw) { reveal("round-trip") }'
        ' otherwise { reveal("mismatch") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == [
        payload.hex(),
        base64.b64encode(payload).decode(),
        "round-trip",
    ]


def test_hex_decode_rejects_odd_and_invalid_input(aphelion_executable, tmp_path):
    odd = run_source(aphelion_executable, tmp_path, 'reveal(acsl.hex_decode("abc"))\n')
    assert odd.returncode == 1 and "even length" in odd.stderr
    invalid = run_source(aphelion_executable, tmp_path, 'reveal(acsl.hex_decode("zz"))\n')
    assert invalid.returncode == 1 and "non-hex character" in invalid.stderr


def test_base64_decode_rejects_malformed_input(aphelion_executable, tmp_path):
    for value, message in (("abc", "multiple of four"), ("ab!d", "invalid character"),
                           ("a===", "padding")):
        result = run_source(aphelion_executable, tmp_path,
                            f'reveal(acsl.base64_decode("{value}"))\n')
        assert result.returncode == 1, value
        assert message in result.stderr


# --------------------------------------------------------------------------
# Authenticated encryption
# --------------------------------------------------------------------------

ZERO_KEY = "00" * 32
ZERO_NONCE = "00" * 12
AES_KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
CHACHA_KEY = "".join(f"{value:02x}" for value in range(0x80, 0xA0))
CHACHA_NONCE = "070000004041424344454647"


def encrypt_hex(executable, tmp_path, plaintext, key_hex, *, function,
                nonce_hex, name="encrypt.agne"):
    body = (
        f'forge key = acsl.key_from_hex({text_literal(key_hex)})\n'
        f'forge nonce = acsl.key_from_hex({text_literal(nonce_hex)})\n'
        f'reveal(acsl.hex_encode(acsl.{function}'
        f'({text_literal(plaintext)}, key, nonce)))\n'
    )
    return bytes.fromhex(single(run_source(executable, tmp_path, body, name=name)))


def test_aes_256_gcm_matches_nist_empty_plaintext_vector(aphelion_executable, tmp_path):
    envelope = encrypt_hex(aphelion_executable, tmp_path, "", ZERO_KEY,
                           function="encrypt_aes256gcm_with_nonce",
                           nonce_hex=ZERO_NONCE)
    assert envelope[:8] == bytes.fromhex("4143534c01010c10")
    assert envelope[8:20] == bytes(12)
    assert envelope[20:36].hex() == "530f8afbc74536b9a963b4f1c4cb738b"
    assert envelope[36:] == b""


def test_aes_256_gcm_matches_reference_ciphertext(aphelion_executable, tmp_path):
    plaintext = "ACSL authenticated encryption"
    envelope = encrypt_hex(aphelion_executable, tmp_path, plaintext, AES_KEY,
                           function="encrypt_aes256gcm_with_nonce",
                           nonce_hex=ZERO_NONCE)
    assert envelope[20:36].hex() == "264e57b4f7c65859993ad1c2bc36ec58"
    assert envelope[36:].hex() == \
        "4fffe692954df6c960cdc741714ff0edb727763646e21256f0eadaecab"


def test_chacha20_poly1305_matches_reference_ciphertext(aphelion_executable, tmp_path):
    plaintext = ("Ladies and Gentlemen of the class of '99: If I could offer you "
                 "only one tip for the future, sunscreen would be it.")
    envelope = encrypt_hex(aphelion_executable, tmp_path, plaintext, CHACHA_KEY,
                           function="encrypt_chacha20poly1305_with_nonce",
                           nonce_hex=CHACHA_NONCE)
    assert envelope[:8] == bytes.fromhex("4143534c01020c10")
    assert envelope[20:36].hex() == "6a23a4681fd59456aea1d29f82477216"
    assert envelope[36:].hex() == (
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3"
        "692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7b"
        "c3ff4def08e4b7a9de576d26586cec64b6116")


@pytest.mark.parametrize("function", ["encrypt", "encrypt_aes256gcm",
                                      "encrypt_chacha20poly1305"])
def test_encryption_round_trips_and_uses_fresh_nonces(aphelion_executable, tmp_path,
                                                      function):
    body = (
        'forge key = acsl.generate_key(32)\n'
        f'forge first = acsl.{function}("secret message", key)\n'
        f'forge second = acsl.{function}("secret message", key)\n'
        'reveal(acsl.decrypt(first, key))\n'
        'reveal(acsl.decrypt(second, key))\n'
        'when first != second { reveal("fresh") } otherwise { reveal("reused") }\n'
        'reveal(length(acsl.decrypt(first, key)))\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["secret message", "secret message", "fresh",
                             str(len("secret message"))]


def test_empty_plaintext_round_trips(aphelion_executable, tmp_path):
    body = (
        'forge key = acsl.generate_key(32)\n'
        'forge sealed = acsl.encrypt("", key)\n'
        'reveal(length(sealed))\n'
        'reveal(length(acsl.decrypt(sealed, key)))\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["36", "0"]


def test_default_encrypt_uses_aes_256_gcm(aphelion_executable, tmp_path):
    body = (
        f'forge key = acsl.key_from_hex({text_literal(ZERO_KEY)})\n'
        'reveal(acsl.hex_encode(acsl.encrypt("", key)))\n'
    )
    envelope = bytes.fromhex(single(run_source(aphelion_executable, tmp_path, body)))
    assert envelope[:8] == bytes.fromhex("4143534c01010c10")


def decrypt_program(ciphertext_hex, key_hex, function="decrypt"):
    return (
        f'forge key = acsl.key_from_hex({text_literal(key_hex)})\n'
        f'forge sealed = acsl.hex_decode({text_literal(ciphertext_hex)})\n'
        f'reveal(acsl.{function}(sealed, key))\n'
    )


def test_tampering_any_envelope_region_fails_authentication(aphelion_executable,
                                                            tmp_path):
    plaintext = "authenticate me"
    envelope = encrypt_hex(aphelion_executable, tmp_path, plaintext, AES_KEY,
                           function="encrypt_aes256gcm_with_nonce",
                           nonce_hex=ZERO_NONCE)
    # Flip one bit in the nonce, the tag, and the ciphertext.
    for index in (8, 20, 36):
        tampered = bytearray(envelope)
        tampered[index] ^= 0x01
        result = run_source(aphelion_executable, tmp_path,
                            decrypt_program(tampered.hex(), AES_KEY),
                            name=f"tamper-{index}.agne")
        assert result.returncode == 1, index
        assert "authentication failed" in result.stderr


def test_wrong_key_fails_authentication(aphelion_executable, tmp_path):
    envelope = encrypt_hex(aphelion_executable, tmp_path, "secret", AES_KEY,
                           function="encrypt_aes256gcm_with_nonce",
                           nonce_hex=ZERO_NONCE)
    other_key = "ff" * 32
    result = run_source(aphelion_executable, tmp_path,
                        decrypt_program(envelope.hex(), other_key))
    assert result.returncode == 1
    assert "authentication failed" in result.stderr


@pytest.mark.parametrize(("mutate", "message"), [
    (lambda data: data[:35], "truncated"),
    (lambda data: b"XXXX" + data[4:], "not an ACSL envelope"),
    (lambda data: data[:4] + b"\x09" + data[5:], "unsupported ACSL envelope version"),
    (lambda data: data[:6] + b"\x10" + data[7:],
     "unsupported ACSL envelope nonce or tag length"),
    (lambda data: data[:5] + b"\x03" + data[6:], "unsupported encryption algorithm"),
])
def test_malformed_envelopes_are_rejected(aphelion_executable, tmp_path, mutate,
                                          message):
    envelope = encrypt_hex(aphelion_executable, tmp_path, "payload", AES_KEY,
                           function="encrypt_aes256gcm_with_nonce",
                           nonce_hex=ZERO_NONCE)
    result = run_source(aphelion_executable, tmp_path,
                        decrypt_program(mutate(envelope).hex(), AES_KEY))
    assert result.returncode == 1
    assert message in result.stderr


def test_algorithm_specific_decrypt_rejects_the_other_algorithm(aphelion_executable,
                                                               tmp_path):
    envelope = encrypt_hex(aphelion_executable, tmp_path, "payload", CHACHA_KEY,
                           function="encrypt_chacha20poly1305_with_nonce",
                           nonce_hex=CHACHA_NONCE)
    result = run_source(
        aphelion_executable, tmp_path,
        decrypt_program(envelope.hex(), CHACHA_KEY, "decrypt_aes256gcm"))
    assert result.returncode == 1
    assert "not the requested algorithm" in result.stderr


@pytest.mark.parametrize(("key", "message"), [
    ('"short"', "requires a 32-byte key"),
    ('acsl.key_from_hex("' + "00" * 31 + '")', "requires a 32-byte key"),
    ('acsl.key_from_hex("' + "00" * 33 + '")', "requires a 32-byte key"),
])
def test_invalid_key_lengths_are_rejected(aphelion_executable, tmp_path, key, message):
    result = run_source(aphelion_executable, tmp_path,
                        f'reveal(acsl.encrypt("data", {key}))\n')
    assert result.returncode == 1
    assert message in result.stderr


def test_invalid_nonce_length_is_rejected(aphelion_executable, tmp_path):
    body = (
        f'forge key = acsl.key_from_hex({text_literal(AES_KEY)})\n'
        'reveal(acsl.encrypt_aes256gcm_with_nonce("data", key, "eleven-byte"))\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert result.returncode == 1
    assert "requires a 12-byte nonce" in result.stderr


# --------------------------------------------------------------------------
# Randomness and key generation
# --------------------------------------------------------------------------

def test_random_bytes_lengths_and_independence(aphelion_executable, tmp_path):
    body = (
        'reveal(length(acsl.random_bytes(0)))\n'
        'reveal(length(acsl.random_bytes(32)))\n'
        'reveal(length(acsl.random_bytes(1024)))\n'
        'when acsl.random_bytes(32) != acsl.random_bytes(32)'
        ' { reveal("distinct") } otherwise { reveal("repeated") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["0", "32", "1024", "distinct"]


def test_random_token_is_lowercase_hex(aphelion_executable, tmp_path):
    body = (
        'forge token = acsl.random_token(64)\n'
        'reveal(length(token))\n'
        'when acsl.constant_time_equal(token, acsl.hex_encode(acsl.hex_decode(token)))'
        ' { reveal("hex") } otherwise { reveal("other") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["64", "hex"]


def test_random_int_stays_within_bounds(aphelion_executable, tmp_path):
    body = (
        'shift i = 0\n'
        'cycle i < 200 {\n'
        '    forge value = acsl.random_int(-5, 5)\n'
        '    assert(value >= -5)\n'
        '    assert(value <= 5)\n'
        '    i += 1\n'
        '}\n'
        'reveal(acsl.random_int(7, 7))\n'
        'reveal(acsl.random_int(-1000000000000, 1000000000000) != 0)\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    values = lines(result)
    assert values[0] == "7"
    assert values[1] in ("bright", "dim")


def test_random_int_rejects_reversed_bounds(aphelion_executable, tmp_path):
    result = run_source(aphelion_executable, tmp_path,
                        "reveal(acsl.random_int(5, 4))\n")
    assert result.returncode == 1
    assert "less than or equal" in result.stderr


@pytest.mark.parametrize("count", ["0", "1025", "-1"])
def test_generate_key_rejects_invalid_lengths(aphelion_executable, tmp_path, count):
    result = run_source(aphelion_executable, tmp_path,
                        f"reveal(length(acsl.generate_key({count})))\n")
    assert result.returncode == 1
    assert "key length" in result.stderr or "between 0 and" in result.stderr


def test_generate_key_and_validate_key(aphelion_executable, tmp_path):
    body = (
        'forge key = acsl.generate_key(32)\n'
        'reveal(length(key))\n'
        'when acsl.validate_key(key, 32) { reveal("valid") } otherwise { reveal("bad") }\n'
        'when acsl.validate_key(key, 31) { reveal("valid") } otherwise { reveal("bad") }\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["32", "valid", "bad"]


# --------------------------------------------------------------------------
# Password-based key derivation
# --------------------------------------------------------------------------

def test_pbkdf2_sha256_matches_hashlib(aphelion_executable, tmp_path):
    expected = hashlib.pbkdf2_hmac("sha256", b"password", b"saltsalt", 4096, 32)
    result = run_source(
        aphelion_executable, tmp_path,
        'reveal(acsl.key_to_hex(acsl.pbkdf2_sha256("password", "saltsalt", 4096, 32)))\n')
    assert single(result) == expected.hex()


def test_argon2id_matches_published_vector(aphelion_executable, tmp_path):
    result = run_source(
        aphelion_executable, tmp_path,
        'reveal(acsl.key_to_hex(acsl.argon2id("password", "somesalt", 2, 65536, 1, 32)))\n')
    assert single(result) == \
        "09316115d5cf24ed5a15a31a3ba326e5cf32edc24702987c02b6566f61913cf7"


def test_derive_key_is_deterministic_and_salt_dependent(aphelion_executable, tmp_path):
    body = (
        'forge first = acsl.key_to_hex(acsl.derive_key("password", "somesaltsomesalt"))\n'
        'forge again = acsl.key_to_hex(acsl.derive_key("password", "somesaltsomesalt"))\n'
        'forge other = acsl.key_to_hex(acsl.derive_key("password", "othersaltothersalt"))\n'
        'when first == again { reveal("stable") } otherwise { reveal("unstable") }\n'
        'when first != other { reveal("salted") } otherwise { reveal("collision") }\n'
        'reveal(length(first))\n'
    )
    result = run_source(aphelion_executable, tmp_path, body)
    assert lines(result) == ["stable", "salted", "64"]


def test_derive_key_requires_a_long_salt(aphelion_executable, tmp_path):
    result = run_source(aphelion_executable, tmp_path,
                        'reveal(acsl.derive_key("password", "short"))\n')
    assert result.returncode == 1
    assert "at least 16 bytes" in result.stderr


@pytest.mark.parametrize(("body", "message"), [
    ('reveal(acsl.argon2id("p", "somesalt", 0, 65536, 1, 32))\n', "time cost"),
    ('reveal(acsl.argon2id("p", "somesalt", 1, 4, 1, 32))\n', "memory cost"),
    ('reveal(acsl.argon2id("p", "somesalt", 1, 65536, 0, 32))\n', "parallelism"),
    ('reveal(acsl.argon2id("p", "short", 1, 65536, 1, 32))\n', "salt must be"),
    ('reveal(acsl.pbkdf2_sha256("p", "short", 1, 32))\n', "salt must be"),
    ('reveal(acsl.pbkdf2_sha256("p", "saltsalt", 0, 32))\n', "iteration count"),
])
def test_kdf_parameter_validation(aphelion_executable, tmp_path, body, message):
    result = run_source(aphelion_executable, tmp_path, body)
    assert result.returncode == 1
    assert message in result.stderr


# --------------------------------------------------------------------------
# Documented demo program
# --------------------------------------------------------------------------

def test_documented_demo_runs_end_to_end(aphelion_executable):
    demo = ROOT / "examples" / "acsl_demo.agne"
    result = subprocess.run([str(aphelion_executable), "run", str(demo)],
                            cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", timeout=120)
    assert result.returncode == 0, result.stderr
    values = result.stdout.splitlines()
    assert values[0] == "sha256: " + hashlib.sha256(b"aphelion").hexdigest()
    assert "decrypted: secret message" in values
    assert "authenticated: bright" in values
    assert "tamper rejected" in result.stdout


# --------------------------------------------------------------------------
# C-level allocation-failure and validation checks
# --------------------------------------------------------------------------

@pytest.fixture(scope="session")
def acsl_checks_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("acsl-checks")
    support = ROOT / "tests" / "support"
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
             "-Werror", *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}",
             *crypto_include_flags()]
    objects = []
    for source in sorted((ROOT / "src").glob("*.c")):
        if source.name == "main.c":
            continue
        output = build / (source.stem + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(source), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", timeout=180)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("acsl-checks")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"),
                             str(support / "acsl_checks.c"), *math_libraries(),
                             *crypto_libraries(), "-o", str(executable)],
                            cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", timeout=180)
    assert result.returncode == 0, result.stderr
    return executable


def test_allocation_failures_release_acsl_buffers(acsl_checks_executable):
    result = subprocess.run([str(acsl_checks_executable)], capture_output=True,
                            text=True, encoding="utf-8", timeout=300)
    assert result.returncode == 0, result.stderr[-4000:]
    assert "ACSL allocation and validation checks passed" in result.stdout
