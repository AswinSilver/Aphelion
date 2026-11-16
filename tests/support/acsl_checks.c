/*
 * Allocation-failure and runtime-validation checks for the ACSL native module.
 *
 * Every allocation point inside a native call is failed in turn.  A failed
 * call must report an error, leave a hollow result, and release every buffer
 * it owns.  The driver links the production sources with the test allocator,
 * so leaks show up as a non-zero live count.
 */

#include "fail_alloc.h"

#include "aphelion/native.h"

#include <stdio.h>
#include <string.h>

#define OK 1
#define CLEAN_FAILURE 0
#define BUG (-1)

static const unsigned char KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static int text_equals(const AphelionValue *value, const char *expected) {
    size_t length = strlen(expected);
    return value->type == APHELION_VALUE_TEXT &&
           value->as.text != NULL &&
           value->as.text->length == length &&
           memcmp(value->as.text->bytes, expected, length) == 0;
}

static AphelionValue text_value(const void *bytes, size_t length) {
    return aphelion_value_text_n((const char *)bytes, length);
}

static int call(unsigned index, AphelionValue *arguments, size_t argument_count,
                AphelionValue *result, char *error, size_t error_size) {
    return aphelion_native_call(index, arguments, argument_count, result,
                                error, error_size) ? OK : CLEAN_FAILURE;
}

static int find(const char *name) {
    int index = aphelion_native_find("acsl", name);
    return index;
}

/* --------------------------------------------------------------------- */

static int body_sha256(size_t failure) {
    AphelionValue arguments[1];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    (void)failure;
    arguments[0] = text_value("abc", 3u);
    if (arguments[0].type != APHELION_VALUE_TEXT) return CLEAN_FAILURE;
    status = call((unsigned)find("sha256_hex"), arguments, 1u, &result,
                  error, sizeof(error));
    if (status == OK && !text_equals(&result,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
        status = BUG;
    }
    if (status != OK && error[0] == '\0') status = BUG;
    if (status != OK && result.type != APHELION_VALUE_HOLLOW) status = BUG;
    aphelion_value_destroy(&result);
    aphelion_value_destroy(&arguments[0]);
    return status;
}

static int body_hmac(size_t failure) {
    AphelionValue arguments[2];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    (void)failure;
    arguments[0] = text_value("Jefe", 4u);
    arguments[1] = text_value("what do ya want for nothing?", 28u);
    if (arguments[0].type != APHELION_VALUE_TEXT ||
        arguments[1].type != APHELION_VALUE_TEXT) {
        aphelion_value_destroy(&arguments[0]);
        aphelion_value_destroy(&arguments[1]);
        return CLEAN_FAILURE;
    }
    status = call((unsigned)find("hmac_sha256_hex"), arguments, 2u, &result,
                  error, sizeof(error));
    if (status == OK && !text_equals(&result,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")) {
        status = BUG;
    }
    if (status != OK && (error[0] == '\0' ||
                         result.type != APHELION_VALUE_HOLLOW)) {
        status = BUG;
    }
    aphelion_value_destroy(&result);
    aphelion_value_destroy(&arguments[1]);
    aphelion_value_destroy(&arguments[0]);
    return status;
}

static int body_encrypt_decrypt(size_t failure) {
    AphelionValue arguments[2];
    AphelionValue sealed = aphelion_value_hollow();
    AphelionValue opened = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    (void)failure;
    arguments[0] = text_value("hello", 5u);
    arguments[1] = text_value(KEY, sizeof(KEY));
    if (arguments[0].type != APHELION_VALUE_TEXT ||
        arguments[1].type != APHELION_VALUE_TEXT) {
        aphelion_value_destroy(&arguments[0]);
        aphelion_value_destroy(&arguments[1]);
        return CLEAN_FAILURE;
    }
    status = call((unsigned)find("encrypt"), arguments, 2u, &sealed,
                  error, sizeof(error));
    if (status == OK) {
        AphelionValue decrypt_arguments[2];
        decrypt_arguments[0] = aphelion_value_move(&sealed);
        decrypt_arguments[1] = aphelion_value_copy(arguments[1]);
        if (decrypt_arguments[1].type != APHELION_VALUE_TEXT) {
            status = CLEAN_FAILURE;
        } else {
            status = call((unsigned)find("decrypt"), decrypt_arguments, 2u,
                          &opened, error, sizeof(error));
            if (status == OK && !text_equals(&opened, "hello")) status = BUG;
            if (status != OK && (error[0] == '\0' ||
                                 opened.type != APHELION_VALUE_HOLLOW)) {
                status = BUG;
            }
        }
        aphelion_value_destroy(&decrypt_arguments[1]);
        aphelion_value_destroy(&decrypt_arguments[0]);
    }
    if (status != OK && error[0] == '\0') status = BUG;
    aphelion_value_destroy(&opened);
    aphelion_value_destroy(&sealed);
    aphelion_value_destroy(&arguments[1]);
    aphelion_value_destroy(&arguments[0]);
    return status;
}

static int body_random(size_t failure) {
    AphelionValue arguments[1];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    (void)failure;
    arguments[0] = aphelion_value_whole(32);
    status = call((unsigned)find("random_bytes"), arguments, 1u, &result,
                  error, sizeof(error));
    if (status == OK &&
        (result.type != APHELION_VALUE_TEXT || result.as.text == NULL ||
         result.as.text->length != 32u)) {
        status = BUG;
    }
    if (status != OK && (error[0] == '\0' ||
                         result.type != APHELION_VALUE_HOLLOW)) {
        status = BUG;
    }
    aphelion_value_destroy(&result);
    return status;
}

static int body_argon2id(size_t failure) {
    AphelionValue arguments[6];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    size_t i;
    (void)failure;
    arguments[0] = text_value("password", 8u);
    arguments[1] = text_value("somesalt", 8u);
    arguments[2] = aphelion_value_whole(1);
    arguments[3] = aphelion_value_whole(8);
    arguments[4] = aphelion_value_whole(1);
    arguments[5] = aphelion_value_whole(32);
    if (arguments[0].type != APHELION_VALUE_TEXT ||
        arguments[1].type != APHELION_VALUE_TEXT) {
        for (i = 0u; i < 2u; ++i) aphelion_value_destroy(&arguments[i]);
        return CLEAN_FAILURE;
    }
    status = call((unsigned)find("argon2id"), arguments, 6u, &result,
                  error, sizeof(error));
    if (status == OK &&
        (result.type != APHELION_VALUE_TEXT || result.as.text == NULL ||
         result.as.text->length != 32u)) {
        status = BUG;
    }
    if (status != OK && (error[0] == '\0' ||
                         result.type != APHELION_VALUE_HOLLOW)) {
        status = BUG;
    }
    aphelion_value_destroy(&result);
    for (i = 0u; i < 2u; ++i) aphelion_value_destroy(&arguments[i]);
    return status;
}

static int body_pbkdf2(size_t failure) {
    AphelionValue arguments[4];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};
    int status;
    (void)failure;
    arguments[0] = text_value("password", 8u);
    arguments[1] = text_value("saltsalt", 8u);
    arguments[2] = aphelion_value_whole(1000);
    arguments[3] = aphelion_value_whole(32);
    if (arguments[0].type != APHELION_VALUE_TEXT ||
        arguments[1].type != APHELION_VALUE_TEXT) {
        aphelion_value_destroy(&arguments[0]);
        aphelion_value_destroy(&arguments[1]);
        return CLEAN_FAILURE;
    }
    status = call((unsigned)find("pbkdf2_sha256"), arguments, 4u, &result,
                  error, sizeof(error));
    if (status == OK &&
        (result.type != APHELION_VALUE_TEXT || result.as.text == NULL ||
         result.as.text->length != 32u)) {
        status = BUG;
    }
    if (status != OK && (error[0] == '\0' ||
                         result.type != APHELION_VALUE_HOLLOW)) {
        status = BUG;
    }
    aphelion_value_destroy(&result);
    aphelion_value_destroy(&arguments[1]);
    aphelion_value_destroy(&arguments[0]);
    return status;
}

/* --------------------------------------------------------------------- */

static int sweep(const char *label, int (*body)(size_t)) {
    size_t attempts;
    size_t point;

    test_alloc_reset(0u);
    if (body(0u) != OK) {
        fprintf(stderr, "%s failed without an allocation failure\n", label);
        return BUG;
    }
    attempts = test_alloc_attempts();
    if (test_alloc_live() != 0u) {
        fprintf(stderr, "%s leaked %zu allocations\n", label, test_alloc_live());
        return BUG;
    }
    for (point = 1u; point <= attempts; ++point) {
        int status;
        test_alloc_reset(point);
        status = body(point);
        if (status == BUG) {
            fprintf(stderr, "%s misbehaved at allocation %zu\n", label, point);
            return BUG;
        }
        if (test_alloc_live() != 0u) {
            fprintf(stderr, "%s leaked %zu allocations at point %zu\n", label,
                    test_alloc_live(), point);
            return BUG;
        }
    }
    printf("%s swept %zu allocation failures\n", label, attempts);
    return 0;
}

static int registry_checks(void) {
    unsigned count = aphelion_native_function_count();
    unsigned i, j;

    if (count == 0u) {
        fputs("native registry is empty\n", stderr);
        return BUG;
    }
    for (i = 0u; i < count; ++i) {
        const AphelionNativeFunction *function = aphelion_native_function_at(i);
        if (function == NULL || function->module == NULL ||
            function->name == NULL || function->signature == NULL ||
            function->invoke == NULL || function->arity == 0u ||
            function->arity > APHELION_NATIVE_MAX_ARITY) {
            fprintf(stderr, "native registry entry %u is malformed\n", i);
            return BUG;
        }
        if (aphelion_native_find(function->module, function->name) != (int)i) {
            fprintf(stderr, "native registry lookup %u disagrees\n", i);
            return BUG;
        }
        for (j = i + 1u; j < count; ++j) {
            const AphelionNativeFunction *other = aphelion_native_function_at(j);
            if (other != NULL && strcmp(function->module, other->module) == 0 &&
                strcmp(function->name, other->name) == 0) {
                fprintf(stderr, "duplicate native function %s\n", function->name);
                return BUG;
            }
        }
    }
    if (aphelion_native_find("acsl", "no_such_function") != -1 ||
        aphelion_native_find("other", "sha256") != -1) {
        fputs("native registry lookup invented an entry\n", stderr);
        return BUG;
    }
    return 0;
}

static int validation_checks(void) {
    AphelionValue arguments[2];
    AphelionValue result = aphelion_value_hollow();
    char error[256] = {0};

    /* Arity is rechecked at runtime. */
    arguments[0] = aphelion_value_whole(1);
    if (aphelion_native_call((unsigned)find("sha256_hex"), arguments, 1u,
                             &result, error, sizeof(error)) ||
        error[0] == '\0' || result.type != APHELION_VALUE_HOLLOW) {
        fputs("runtime arity validation failed\n", stderr);
        return BUG;
    }

    /* Types are rechecked at runtime. */
    error[0] = '\0';
    if (aphelion_native_call((unsigned)find("sha256_hex"), arguments, 1u,
                             &result, error, sizeof(error)) ||
        strstr(error, "text") == NULL) {
        fputs("runtime type validation failed\n", stderr);
        return BUG;
    }

    /* Invalid indexes are rejected without touching the result. */
    error[0] = '\0';
    if (aphelion_native_call(aphelion_native_function_count(), arguments, 1u,
                             &result, error, sizeof(error)) ||
        error[0] == '\0') {
        fputs("runtime index validation failed\n", stderr);
        return BUG;
    }

    /* Wrong key length is reported without leaking key material. */
    arguments[0] = text_value("hello", 5u);
    arguments[1] = text_value("short", 5u);
    if (arguments[0].type == APHELION_VALUE_TEXT &&
        arguments[1].type == APHELION_VALUE_TEXT) {
        error[0] = '\0';
        if (aphelion_native_call((unsigned)find("encrypt"), arguments, 2u,
                                 &result, error, sizeof(error)) ||
            strstr(error, "32-byte key") == NULL ||
            strstr(error, "short") != NULL) {
            fputs("wrong-key diagnostic failed\n", stderr);
            return BUG;
        }
    }
    aphelion_value_destroy(&arguments[1]);
    aphelion_value_destroy(&arguments[0]);

    /* A tampered envelope must fail authentication and free its buffers. */
    {
        AphelionValue plain[2];
        AphelionValue sealed = aphelion_value_hollow();
        AphelionValue opened = aphelion_value_hollow();
        plain[0] = text_value("tamper me", 9u);
        plain[1] = text_value(KEY, sizeof(KEY));
        if (plain[0].type != APHELION_VALUE_TEXT ||
            plain[1].type != APHELION_VALUE_TEXT) {
            aphelion_value_destroy(&plain[0]);
            aphelion_value_destroy(&plain[1]);
            return BUG;
        }
        if (!aphelion_native_call((unsigned)find("encrypt"), plain, 2u,
                                  &sealed, error, sizeof(error))) {
            aphelion_value_destroy(&plain[0]);
            aphelion_value_destroy(&plain[1]);
            return BUG;
        }
        sealed.as.text->bytes[sealed.as.text->length - 1u] ^= 0x01;
        {
            AphelionValue decrypt_arguments[2];
            decrypt_arguments[0] = aphelion_value_move(&sealed);
            decrypt_arguments[1] = aphelion_value_copy(plain[1]);
            error[0] = '\0';
            if (aphelion_native_call((unsigned)find("decrypt"),
                                     decrypt_arguments, 2u, &opened,
                                     error, sizeof(error)) ||
                strstr(error, "authentication failed") == NULL) {
                fputs("tamper detection failed\n", stderr);
                aphelion_value_destroy(&decrypt_arguments[0]);
                aphelion_value_destroy(&decrypt_arguments[1]);
                aphelion_value_destroy(&plain[0]);
                aphelion_value_destroy(&plain[1]);
                return BUG;
            }
            aphelion_value_destroy(&decrypt_arguments[1]);
            aphelion_value_destroy(&decrypt_arguments[0]);
        }
        aphelion_value_destroy(&opened);
        aphelion_value_destroy(&sealed);
        aphelion_value_destroy(&plain[1]);
        aphelion_value_destroy(&plain[0]);
    }
    aphelion_value_destroy(&result);
    return 0;
}

int main(void) {
    if (registry_checks() != 0) return 1;
    if (sweep("sha256", body_sha256) != 0) return 1;
    if (sweep("hmac", body_hmac) != 0) return 1;
    if (sweep("encrypt", body_encrypt_decrypt) != 0) return 1;
    if (sweep("random", body_random) != 0) return 1;
    if (sweep("argon2id", body_argon2id) != 0) return 1;
    if (sweep("pbkdf2", body_pbkdf2) != 0) return 1;
    test_alloc_reset(0u);
    if (validation_checks() != 0) return 1;
    if (test_alloc_live() != 0u) {
        fprintf(stderr, "validation checks leaked %zu allocations\n",
                test_alloc_live());
        return 1;
    }
    puts("ACSL allocation and validation checks passed");
    return 0;
}
