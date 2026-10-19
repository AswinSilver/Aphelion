#include "fail_alloc.h"
#include "aphelion/vm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(APHELION_VALUE_UNINITIALIZED == 8 && APHELION_VALUE_LIST == 9,
               "existing value tags must not change");
_Static_assert(APHELION_OP_ASSERT == 42 && APHELION_OP_LIST == 43 &&
               APHELION_OP_INDEX == 44, "existing opcode numbers must not change");
_Static_assert(APHELION_BYTECODE_VERSION == 1u, "preserve version one");

static int values(size_t failure) {
    AphelionValue text = aphelion_value_hollow();
    AphelionValue list = aphelion_value_hollow(), copy = aphelion_value_hollow();
    AphelionValue moved = aphelion_value_hollow(), empty = aphelion_value_hollow();
    AphelionValue typed_empty = aphelion_value_hollow(), other = aphelion_value_hollow();
    AphelionValue items[2], invalid, whole = aphelion_value_whole(42);
    char *printed = NULL;
    FILE *output = tmpfile();
    int ok = 0;
    if (output == NULL) return 0;
    test_alloc_reset(failure);
    text = aphelion_value_text_n("a\0\"\\\n\r\t\001\177", 9u);
    if (text.type != APHELION_VALUE_TEXT) goto cleanup;
    items[0] = text;
    items[1] = text;
    list = aphelion_value_list(APHELION_VALUE_TEXT, items, 2u);
    if (list.type != APHELION_VALUE_LIST) goto cleanup;
    assert(text.as.text->references == 3u);
    assert(list.as.list->references == 1u && list.as.list->length == 2u);
    assert(list.as.list->element_type == APHELION_VALUE_TEXT);
    assert(aphelion_value_truthy(list));
    assert(strcmp(aphelion_value_type_name(list.type), "list") == 0);
    copy = aphelion_value_copy(list);
    moved = aphelion_value_move(&copy);
    assert(copy.type == APHELION_VALUE_HOLLOW && list.as.list->references == 2u);
    aphelion_value_destroy(&list);
    assert(moved.as.list->references == 1u);
    aphelion_value_destroy(&text);
    assert(moved.as.list->items[0].as.text->references == 2u);
    /* Changing the source vector must not change the retained-copy storage. */
    items[0] = whole;
    items[1] = whole;
    printed = aphelion_value_to_string(moved);
    if (printed == NULL) goto cleanup;
    assert(strcmp(printed, "[\"a\\0\\\"\\\\\\n\\r\\t\\x01\\x7f\", \"a\\0\\\"\\\\\\n\\r\\t\\x01\\x7f\"]") == 0);
    if (!aphelion_value_write(output, moved)) goto cleanup;
    rewind(output);
    {
        char buffer[256];
        size_t length = fread(buffer, 1u, sizeof(buffer) - 1u, output);
        buffer[length] = '\0';
        assert(strcmp(buffer, printed) == 0);
    }
    empty = aphelion_value_list(APHELION_VALUE_HOLLOW, NULL, 0u);
    typed_empty = aphelion_value_list(APHELION_VALUE_TEXT, NULL, 0u);
    if (empty.type != APHELION_VALUE_LIST || typed_empty.type != APHELION_VALUE_LIST) goto cleanup;
    assert(aphelion_value_equal(empty, typed_empty));
    assert(!aphelion_value_equal(moved, empty) && !aphelion_value_truthy(empty));
    other = aphelion_value_list(APHELION_VALUE_TEXT, moved.as.list->items, 2u);
    if (other.type != APHELION_VALUE_LIST) goto cleanup;
    assert(aphelion_value_equal(moved, other));
    invalid = aphelion_value_list(APHELION_VALUE_LIST, &moved, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_list(APHELION_VALUE_HOLLOW, NULL, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_list(APHELION_VALUE_WHOLE, NULL, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_list(APHELION_VALUE_WHOLE, &whole, SIZE_MAX);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_list(APHELION_VALUE_DECIMAL, &whole, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    items[0] = moved.as.list->items[0];
    items[1] = whole;
    {
        size_t references = items[0].as.text->references;
        invalid = aphelion_value_list(APHELION_VALUE_TEXT, items, 2u);
        assert(invalid.type == APHELION_VALUE_HOLLOW);
        assert(items[0].as.text->references == references);
    }
    {
        AphelionValueType bad[] = { APHELION_VALUE_FUNCTION, APHELION_VALUE_SILENT,
                                   APHELION_VALUE_UNINITIALIZED, (AphelionValueType)-1 };
        size_t i;
        for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            invalid = aphelion_value_list(bad[i], NULL, 0u);
            assert(invalid.type == APHELION_VALUE_HOLLOW);
        }
    }
    invalid = aphelion_value_hollow();
    invalid.type = APHELION_VALUE_TEXT;
    invalid = aphelion_value_list(APHELION_VALUE_TEXT, &invalid, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_decimal(INFINITY);
    invalid = aphelion_value_list(APHELION_VALUE_DECIMAL, &invalid, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    invalid = aphelion_value_glyph(0xd800u);
    invalid = aphelion_value_list(APHELION_VALUE_GLYPH, &invalid, 1u);
    assert(invalid.type == APHELION_VALUE_HOLLOW);
    ok = 1;
cleanup:
    free(printed);
    aphelion_value_destroy(&text);
    aphelion_value_destroy(&list);
    aphelion_value_destroy(&copy);
    aphelion_value_destroy(&moved);
    aphelion_value_destroy(&empty);
    aphelion_value_destroy(&typed_empty);
    aphelion_value_destroy(&other);
    fclose(output);
    return ok;
}

static int execute(const char *path, const char *rewrite, size_t failure, int quiet) {
    AphelionBytecode bytecode;
    AphelionVM vm;
    char error[256] = {0};
    FILE *output = quiet ? tmpfile() : stdout;
    int ok;
    if (output == NULL) return 0;
    test_alloc_reset(failure);
    aphelion_bytecode_init(&bytecode);
    aphelion_vm_init(&vm);
    aphelion_vm_set_output(&vm, output);
    aphelion_vm_set_instruction_limit(&vm, 1000u);
    ok = aphelion_bytecode_read_file(&bytecode, path, error, sizeof(error));
    if (ok && rewrite != NULL) {
        ok = aphelion_bytecode_write_file(&bytecode, rewrite, error, sizeof(error));
    }
    if (ok) {
        ok = aphelion_vm_run(&vm, &bytecode);
        if (!ok) {
            assert(aphelion_vm_error(&vm)->message[0] != '\0');
            if (!quiet) fprintf(stderr, "%s\n", aphelion_vm_error(&vm)->message);
        }
    } else {
        assert(error[0] != '\0');
        if (!quiet) fprintf(stderr, "%s\n", error);
    }
    aphelion_vm_destroy(&vm);
    aphelion_bytecode_destroy(&bytecode);
    if (quiet) fclose(output);
    return ok;
}

static int invalid_constants(const char *path) {
    AphelionBytecode bytecode;
    AphelionValue value;
    AphelionValueType bad[] = { APHELION_VALUE_LIST, APHELION_VALUE_UNINITIALIZED,
                               (AphelionValueType)-1, (AphelionValueType)255 };
    size_t i;
    char error[256];
    test_alloc_reset(0u);
    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        aphelion_bytecode_init(&bytecode);
        assert(aphelion_bytecode_add_function(&bytecode, "entry", 0u, 0u) == 0);
        assert(aphelion_function_emit(&bytecode.functions[0], APHELION_OP_HALT, 0, 0, 1u, 1u) == 0);
        value = aphelion_value_hollow();
        if (bad[i] == APHELION_VALUE_LIST) {
            value = aphelion_value_list(APHELION_VALUE_HOLLOW, NULL, 0u);
            assert(value.type == APHELION_VALUE_LIST);
        } else value.type = bad[i];
        assert(aphelion_bytecode_add_constant(&bytecode, value) == 0);
        aphelion_value_destroy(&value);
        assert(!aphelion_bytecode_validate(&bytecode, error, sizeof(error)));
        assert(!aphelion_bytecode_write_file(&bytecode, path, error, sizeof(error)));
        aphelion_bytecode_destroy(&bytecode);
    }
    return test_alloc_live() != 0u ? 2 : 0;
}

int main(int argc, char **argv) {
    int ok;
    if (argc == 2 && strcmp(argv[1], "--values") == 0) {
        size_t point, attempts = 0u;
        for (point = 0u; point <= attempts; ++point) {
            ok = values(point);
            if (point == 0u) { assert(ok); attempts = test_alloc_attempts(); }
            else assert(!ok);
            if (test_alloc_live() != 0u) return 2;
        }
        puts("value allocation cleanup checks passed");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--invalid-constants") == 0) {
        return invalid_constants(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "--sweep") == 0) {
        size_t point, attempts = 0u;
        for (point = 0u; point <= attempts; ++point) {
            ok = execute(argv[2], NULL, point, 1);
            if (point == 0u) attempts = test_alloc_attempts();
            else if (ok) {
                fprintf(stderr, "allocation failure %zu was masked\n", point);
                return 3;
            }
            if (test_alloc_live() != 0u) {
                fprintf(stderr, "allocation failure %zu leaked %zu allocations\n", point, test_alloc_live());
                return 2;
            }
        }
        puts("VM allocation cleanup checks passed");
        return 0;
    }
    if (argc < 2 || argc > 3) return 3;
    ok = execute(argv[1], argc == 3 ? argv[2] : NULL, 0u, 0);
    if (test_alloc_live() != 0u) return 2;
    return ok ? 0 : 1;
}
