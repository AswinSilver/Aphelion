#include "aphelion/vm.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void vm_clear_stack(AphelionVM *vm) {
    while (vm->stack_count > 0u) {
        vm->stack_count -= 1u;
        aphelion_value_destroy(&vm->stack[vm->stack_count]);
    }
}

static void vm_clear_globals(AphelionVM *vm) {
    size_t i;
    if (vm->globals != NULL) {
        for (i = 0u; i < vm->global_count; ++i) {
            aphelion_value_destroy(&vm->globals[i]);
        }
    }
    free(vm->globals);
    vm->globals = NULL;
    vm->global_count = 0u;
}

void aphelion_vm_init(AphelionVM *vm) {
    if (vm == NULL) {
        return;
    }
    memset(vm, 0, sizeof(*vm));
    vm->stack_limit = APHELION_VM_DEFAULT_STACK_LIMIT;
    vm->call_limit = APHELION_VM_DEFAULT_CALL_LIMIT;
    vm->instruction_limit = APHELION_VM_DEFAULT_INSTRUCTION_LIMIT;
    vm->input = stdin;
    vm->output = stdout;
}

void aphelion_vm_destroy(AphelionVM *vm) {
    if (vm == NULL) {
        return;
    }
    vm_clear_stack(vm);
    vm_clear_globals(vm);
    free(vm->stack);
    free(vm->frames);
    memset(vm, 0, sizeof(*vm));
}

void aphelion_vm_set_limits(AphelionVM *vm,
                            size_t stack_limit,
                            size_t call_limit) {
    if (vm == NULL) {
        return;
    }
    vm->stack_limit = stack_limit != 0u ? stack_limit : APHELION_VM_DEFAULT_STACK_LIMIT;
    vm->call_limit = call_limit != 0u ? call_limit : APHELION_VM_DEFAULT_CALL_LIMIT;
}

void aphelion_vm_set_input(AphelionVM *vm, FILE *input) {
    if (vm != NULL) {
        vm->input = input != NULL ? input : stdin;
    }
}

void aphelion_vm_set_instruction_limit(AphelionVM *vm, uint64_t limit) {
    if (vm != NULL) vm->instruction_limit = limit != 0u
                                      ? limit : APHELION_VM_DEFAULT_INSTRUCTION_LIMIT;
}

void aphelion_vm_set_output(AphelionVM *vm, FILE *output) {
    if (vm != NULL) {
        vm->output = output != NULL ? output : stdout;
    }
}

static void vm_set_error(AphelionVM *vm, const AphelionInstruction *instruction,
                         const char *format, ...) {
    va_list args;
    if (vm == NULL || vm->failed) {
        return;
    }
    vm->failed = true;
    vm->error.file = vm->bytecode != NULL && vm->bytecode->source_name != NULL
                       ? vm->bytecode->source_name : "<bytecode>";
    if (vm->frame_count != 0u &&
        vm->frames[vm->frame_count - 1u].source_name != NULL) {
        vm->error.file = vm->frames[vm->frame_count - 1u].source_name;
    }
    vm->error.line = instruction != NULL ? instruction->line : 0u;
    vm->error.column = instruction != NULL ? instruction->column : 0u;
    va_start(args, format);
    (void)vsnprintf(vm->error.message, sizeof(vm->error.message), format, args);
    va_end(args);
}

const AphelionRuntimeError *aphelion_vm_error(const AphelionVM *vm) {
    return vm != NULL ? &vm->error : NULL;
}

static bool vm_grow_stack(AphelionVM *vm, size_t count) {
    size_t next;
    AphelionValue *grown;
    if (count > vm->stack_limit) {
        return false;
    }
    if (count <= vm->stack_capacity) {
        return true;
    }
    next = vm->stack_capacity == 0u ? 64u : vm->stack_capacity;
    while (next < count) {
        if (next > vm->stack_limit / 2u) {
            next = vm->stack_limit;
            break;
        }
        next *= 2u;
    }
    if (next < count || next > SIZE_MAX / sizeof(*grown)) {
        return false;
    }
    grown = (AphelionValue *)realloc(vm->stack, next * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    vm->stack = grown;
    vm->stack_capacity = next;
    return true;
}

static bool vm_grow_frames(AphelionVM *vm, size_t count) {
    size_t next;
    AphelionCallFrame *grown;
    if (count > vm->call_limit) {
        return false;
    }
    if (count <= vm->frame_capacity) {
        return true;
    }
    next = vm->frame_capacity == 0u ? 16u : vm->frame_capacity;
    while (next < count) {
        if (next > vm->call_limit / 2u) {
            next = vm->call_limit;
            break;
        }
        next *= 2u;
    }
    if (next < count || next > SIZE_MAX / sizeof(*grown)) {
        return false;
    }
    grown = (AphelionCallFrame *)realloc(vm->frames, next * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    vm->frames = grown;
    vm->frame_capacity = next;
    return true;
}

static bool vm_push_move(AphelionVM *vm, AphelionValue *value,
                         const AphelionInstruction *instruction) {
    if (!vm_grow_stack(vm, vm->stack_count + 1u)) {
        vm_set_error(vm, instruction, "operand stack limit exceeded");
        aphelion_value_destroy(value);
        return false;
    }
    vm->stack[vm->stack_count++] = aphelion_value_move(value);
    return true;
}

static bool vm_push_copy(AphelionVM *vm, AphelionValue value,
                         const AphelionInstruction *instruction) {
    value = aphelion_value_copy(value);
    if (!vm_push_move(vm, &value, instruction)) {
        aphelion_value_destroy(&value);
        return false;
    }
    return true;
}

static size_t vm_operand_count(const AphelionVM *vm) {
    const AphelionCallFrame *frame;
    size_t floor;
    if (vm->frame_count == 0u) return 0u;
    frame = &vm->frames[vm->frame_count - 1u];
    floor = frame->base + vm->bytecode->functions[frame->function_index].local_count;
    return vm->stack_count > floor ? vm->stack_count - floor : 0u;
}

static bool vm_pop_move(AphelionVM *vm, AphelionValue *value,
                         const AphelionInstruction *instruction) {
    if (vm_operand_count(vm) == 0u) {
        vm_set_error(vm, instruction, "operand stack underflow");
        return false;
    }
    vm->stack_count -= 1u;
    *value = aphelion_value_move(&vm->stack[vm->stack_count]);
    return true;
}

static bool vm_list(AphelionVM *vm, const AphelionInstruction *instruction) {
    size_t count = (size_t)instruction->operand;
    size_t start, i;
    AphelionValue result;
    if (count > vm_operand_count(vm)) {
        vm_set_error(vm, instruction, "operand stack underflow in LIST");
        return false;
    }
    start = vm->stack_count - count;
    for (i = start; i < vm->stack_count; ++i) {
        if (vm->stack[i].type != (AphelionValueType)instruction->operand2) {
            vm_set_error(vm, instruction, "LIST requires homogeneous %s elements, got %s",
                         aphelion_value_type_name((AphelionValueType)instruction->operand2),
                         aphelion_value_type_name(vm->stack[i].type));
            return false;
        }
    }
    result = aphelion_value_list((AphelionValueType)instruction->operand2,
                                count != 0u ? vm->stack + start : NULL, count);
    if (result.type != APHELION_VALUE_LIST) {
        vm_set_error(vm, instruction, "out of memory or invalid primitive value creating list");
        return false;
    }
    while (vm->stack_count > start) {
        aphelion_value_destroy(&vm->stack[--vm->stack_count]);
    }
    return vm_push_move(vm, &result, instruction);
}

static bool vm_index(AphelionVM *vm, const AphelionInstruction *instruction) {
    AphelionValue index = aphelion_value_hollow();
    AphelionValue list = aphelion_value_hollow();
    bool ok = false;
    if (!vm_pop_move(vm, &index, instruction) ||
        !vm_pop_move(vm, &list, instruction)) goto cleanup;
    if (list.type != APHELION_VALUE_LIST || list.as.list == NULL) {
        vm_set_error(vm, instruction, "index requires a list target, got %s",
                     aphelion_value_type_name(list.type));
    } else if (index.type != APHELION_VALUE_WHOLE) {
        vm_set_error(vm, instruction, "list index requires whole, got %s",
                     aphelion_value_type_name(index.type));
    } else if (index.as.whole < 0 ||
               (uint64_t)index.as.whole >= (uint64_t)list.as.list->length) {
        vm_set_error(vm, instruction, "list index is out of bounds");
    } else {
        ok = vm_push_copy(vm, list.as.list->items[(size_t)index.as.whole], instruction);
    }
cleanup:
    aphelion_value_destroy(&index);
    aphelion_value_destroy(&list);
    return ok;
}

static bool vm_append_input(char **buffer, size_t *length, size_t *capacity,
                            const char *chunk, size_t chunk_length) {
    size_t needed;
    size_t next;
    char *grown;
    if (chunk_length == 0u) return true;
    if (*length > SIZE_MAX - chunk_length - 1u) return false;
    needed = *length + chunk_length + 1u;
    if (needed > *capacity) {
        next = *capacity == 0u ? 256u : *capacity;
        while (next < needed) {
            if (next > SIZE_MAX / 2u) {
                next = needed;
                break;
            }
            next *= 2u;
        }
        if (next < needed) return false;
        grown = (char *)realloc(*buffer, next);
        if (grown == NULL) return false;
        *buffer = grown;
        *capacity = next;
    }
    memcpy(*buffer + *length, chunk, chunk_length);
    *length += chunk_length;
    (*buffer)[*length] = '\0';
    return true;
}

static bool vm_read_line(AphelionVM *vm, const AphelionInstruction *instruction,
                         AphelionValue *result) {
    char *buffer = NULL;
    size_t length = 0u;
    size_t capacity = 0u;
    FILE *input = vm->input != NULL ? vm->input : stdin;
    int character;

    while ((character = fgetc(input)) != EOF && character != '\n') {
        char byte = (char)character;
        if (!vm_append_input(&buffer, &length, &capacity, &byte, 1u)) {
            free(buffer);
            vm_set_error(vm, instruction, "out of memory reading input");
            return false;
        }
    }
    if (ferror(input)) {
        free(buffer);
        vm_set_error(vm, instruction, "input read failed");
        return false;
    }
    if (character == '\n' && length > 0u && buffer[length - 1u] == '\r') --length;
    *result = aphelion_value_text_n(buffer != NULL ? buffer : "", length);
    free(buffer);
    if (result->type != APHELION_VALUE_TEXT || result->as.text == NULL) {
        vm_set_error(vm, instruction, "out of memory creating input text");
        return false;
    }
    return true;
}

static bool parse_whole_text(AphelionText *text, int64_t *result) {
    size_t i = 0u;
    bool negative = false;
    uint64_t magnitude = 0u;
    uint64_t limit;
    if (text == NULL || text->length == 0u || result == NULL) return false;
    if (text->bytes[i] == '+' || text->bytes[i] == '-') {
        negative = text->bytes[i] == '-';
        if (++i == text->length) return false;
    }
    limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < text->length; ++i) {
        unsigned char digit = (unsigned char)text->bytes[i];
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9') return false;
        digit = (unsigned char)(digit - (unsigned char)'0');
        if (magnitude > (limit - digit) / 10u) return false;
        magnitude = magnitude * 10u + digit;
    }
    if (negative) {
        if (magnitude == (uint64_t)INT64_MAX + 1u) {
            *result = INT64_MIN;
        } else {
            *result = -(int64_t)magnitude;
        }
    } else {
        *result = (int64_t)magnitude;
    }
    return true;
}

static bool parse_decimal_text(AphelionText *text, double *result) {
    char *copy;
    char *end;
    size_t i;
    double value;
    if (text == NULL || text->length == 0u || result == NULL ||
        text->length > SIZE_MAX - 1u) return false;
    for (i = 0u; i < text->length; ++i) {
        if (text->bytes[i] == '\0' || isspace((unsigned char)text->bytes[i])) {
            return false;
        }
    }
    copy = (char *)malloc(text->length + 1u);
    if (copy == NULL) return false;
    memcpy(copy, text->bytes, text->length);
    copy[text->length] = '\0';
    errno = 0;
    value = strtod(copy, &end);
    if (errno == ERANGE || end != copy + text->length || !isfinite(value)) {
        free(copy);
        return false;
    }
    free(copy);
    *result = value;
    return true;
}

static bool vm_to_whole(AphelionVM *vm, AphelionValue input,
                        const AphelionInstruction *instruction,
                        AphelionValue *result) {
    int64_t whole;
    if (input.type == APHELION_VALUE_WHOLE) {
        *result = aphelion_value_copy(input);
        return true;
    }
    if (input.type == APHELION_VALUE_DECIMAL) {
        double truncated;
        if (!isfinite(input.as.decimal)) {
            vm_set_error(vm, instruction, "to_whole requires a finite decimal");
            return false;
        }
        truncated = trunc(input.as.decimal);
        /* Half-open powers-of-two bounds stay exact even when long double
         * has only binary64 precision (e.g. MSVC). */
        if (truncated < -0x1p63 || truncated >= 0x1p63) {
            vm_set_error(vm, instruction, "decimal is out of whole range");
            return false;
        }
        *result = aphelion_value_whole((int64_t)truncated);
        return true;
    }
    if (input.type == APHELION_VALUE_TEXT && parse_whole_text(input.as.text, &whole)) {
        *result = aphelion_value_whole(whole);
        return true;
    }
    vm_set_error(vm, instruction,
                 "cannot convert %s to whole; expected whole, decimal, or numeric text",
                 aphelion_value_type_name(input.type));
    return false;
}

static bool vm_to_decimal(AphelionVM *vm, AphelionValue input,
                          const AphelionInstruction *instruction,
                          AphelionValue *result) {
    double decimal;
    if (input.type == APHELION_VALUE_WHOLE) {
        *result = aphelion_value_decimal((double)input.as.whole);
        return true;
    }
    if (input.type == APHELION_VALUE_DECIMAL && isfinite(input.as.decimal)) {
        *result = aphelion_value_copy(input);
        return true;
    }
    if (input.type == APHELION_VALUE_TEXT &&
        parse_decimal_text(input.as.text, &decimal)) {
        *result = aphelion_value_decimal(decimal);
        return true;
    }
    vm_set_error(vm, instruction,
                 "cannot convert %s to decimal; expected whole, decimal, or numeric text",
                 aphelion_value_type_name(input.type));
    return false;
}

/* Diagnostics must not treat text as a C string: messages may contain NULs.
 * Keep the escaped prefix bounded and allocation-free, even on OOM paths. */
static void vm_assertion_error(AphelionVM *vm,
                               const AphelionInstruction *instruction,
                               const AphelionText *message) {
    char escaped[192];
    static const char hex[] = "0123456789abcdef";
    size_t i = 0u;
    size_t used = 0u;
    if (message == NULL) {
        vm_set_error(vm, instruction, "assertion failed");
        return;
    }
    while (i < message->length && used + 7u < sizeof(escaped)) {
        unsigned char byte = (unsigned char)message->bytes[i++];
        switch (byte) {
        case '\0': escaped[used++] = '\\'; escaped[used++] = '0'; break;
        case '\n': escaped[used++] = '\\'; escaped[used++] = 'n'; break;
        case '\r': escaped[used++] = '\\'; escaped[used++] = 'r'; break;
        case '\t': escaped[used++] = '\\'; escaped[used++] = 't'; break;
        case '\\': escaped[used++] = '\\'; escaped[used++] = '\\'; break;
        default:
            if (byte < 0x20u || byte == 0x7fu) {
                escaped[used++] = '\\';
                escaped[used++] = 'x';
                escaped[used++] = hex[byte >> 4u];
                escaped[used++] = hex[byte & 0x0fu];
            } else {
                escaped[used++] = (char)byte;
            }
            break;
        }
    }
    if (i < message->length) {
        escaped[used++] = '.';
        escaped[used++] = '.';
        escaped[used++] = '.';
    }
    escaped[used] = '\0';
    vm_set_error(vm, instruction, "assertion failed: %s", escaped);
}

static bool vm_assert(AphelionVM *vm,
                       const AphelionInstruction *instruction) {
    AphelionValue signal = aphelion_value_hollow();
    AphelionValue message = aphelion_value_hollow();
    AphelionValue result;
    bool ok = false;
    /* Arguments are emitted left-to-right; the optional message is on top. */
    if (instruction->operand == 2 &&
        !vm_pop_move(vm, &message, instruction)) goto cleanup;
    if (!vm_pop_move(vm, &signal, instruction)) goto cleanup;
    if (signal.type != APHELION_VALUE_SIGNAL) {
        vm_set_error(vm, instruction, "assert requires a signal, got %s",
                     aphelion_value_type_name(signal.type));
    } else if (instruction->operand == 2 &&
               (message.type != APHELION_VALUE_TEXT || message.as.text == NULL)) {
        vm_set_error(vm, instruction, "assert message requires text, got %s",
                     aphelion_value_type_name(message.type));
    } else if (!signal.as.signal) {
        vm_assertion_error(vm, instruction,
                           instruction->operand == 2 ? message.as.text : NULL);
    } else {
        result = aphelion_value_silent();
        ok = vm_push_move(vm, &result, instruction);
    }
cleanup:
    aphelion_value_destroy(&signal);
    aphelion_value_destroy(&message);
    return ok;
}

static bool numeric(AphelionValue value, double *number, bool *whole) {
    if (value.type == APHELION_VALUE_WHOLE) {
        *number = (double)value.as.whole;
        *whole = true;
        return true;
    }
    if (value.type == APHELION_VALUE_DECIMAL) {
        *number = value.as.decimal;
        *whole = false;
        return true;
    }
    return false;
}

static AphelionValue concat_text(AphelionValue left, AphelionValue right) {
    size_t left_length = left.as.text != NULL ? left.as.text->length : 0u;
    size_t right_length = right.as.text != NULL ? right.as.text->length : 0u;
    char *buffer;
    AphelionValue result;
    if (left_length >= SIZE_MAX - right_length) {
        return aphelion_value_hollow();
    }
    buffer = (char *)malloc(left_length + right_length + 1u);
    if (buffer == NULL) {
        return aphelion_value_hollow();
    }
    if (left_length != 0u && left.as.text != NULL) {
        memcpy(buffer, left.as.text->bytes, left_length);
    }
    if (right_length != 0u && right.as.text != NULL) {
        memcpy(buffer + left_length, right.as.text->bytes, right_length);
    }
    buffer[left_length + right_length] = '\0';
    result = aphelion_value_text_n(buffer, left_length + right_length);
    free(buffer);
    return result;
}

static bool compare_values(AphelionValue left, AphelionValue right,
                           int *comparison) {
    double left_number;
    double right_number;
    bool left_whole;
    bool right_whole;
    if (left.type == APHELION_VALUE_WHOLE &&
        right.type == APHELION_VALUE_WHOLE) {
        *comparison = left.as.whole < right.as.whole ? -1
                        : left.as.whole > right.as.whole ? 1 : 0;
        return true;
    }
    if (numeric(left, &left_number, &left_whole) &&
        numeric(right, &right_number, &right_whole)) {
        (void)left_whole;
        (void)right_whole;
        *comparison = left_number < right_number ? -1
                        : left_number > right_number ? 1 : 0;
        return true;
    }
    if (left.type == APHELION_VALUE_TEXT && right.type == APHELION_VALUE_TEXT) {
        const char *a = left.as.text != NULL ? left.as.text->bytes : "";
        const char *b = right.as.text != NULL ? right.as.text->bytes : "";
        size_t a_length = left.as.text != NULL ? left.as.text->length : 0u;
        size_t b_length = right.as.text != NULL ? right.as.text->length : 0u;
        size_t common = a_length < b_length ? a_length : b_length;
        int result = common != 0u ? memcmp(a, b, common) : 0;
        if (result == 0) {
            result = a_length < b_length ? -1 : a_length > b_length ? 1 : 0;
        }
        *comparison = result < 0 ? -1 : result > 0 ? 1 : 0;
        return true;
    }
    if (left.type == APHELION_VALUE_GLYPH && right.type == APHELION_VALUE_GLYPH) {
        *comparison = left.as.glyph < right.as.glyph ? -1
                       : left.as.glyph > right.as.glyph ? 1 : 0;
        return true;
    }
    return false;
}

static bool vm_binary(AphelionVM *vm, AphelionOpcode opcode,
                      AphelionValue left, AphelionValue right,
                      AphelionValue *result,
                      const AphelionInstruction *instruction) {
    double a;
    double b;
    bool a_whole;
    bool b_whole;
    int comparison;
    if (opcode == APHELION_OP_EQUAL || opcode == APHELION_OP_NOT_EQUAL) {
        bool equal = aphelion_value_equal(left, right);
        *result = aphelion_value_signal(opcode == APHELION_OP_EQUAL ? equal : !equal);
        return true;
    }
    if (opcode == APHELION_OP_LESS || opcode == APHELION_OP_LESS_EQUAL ||
        opcode == APHELION_OP_GREATER || opcode == APHELION_OP_GREATER_EQUAL) {
        if (!compare_values(left, right, &comparison)) {
            vm_set_error(vm, instruction, "values of type %s and %s cannot be compared",
                         aphelion_value_type_name(left.type),
                         aphelion_value_type_name(right.type));
            return false;
        }
        switch (opcode) {
        case APHELION_OP_LESS: *result = aphelion_value_signal(comparison < 0); break;
        case APHELION_OP_LESS_EQUAL: *result = aphelion_value_signal(comparison <= 0); break;
        case APHELION_OP_GREATER: *result = aphelion_value_signal(comparison > 0); break;
        default: *result = aphelion_value_signal(comparison >= 0); break;
        }
        return true;
    }
    if (opcode == APHELION_OP_AND || opcode == APHELION_OP_OR) {
        if (left.type != APHELION_VALUE_SIGNAL || right.type != APHELION_VALUE_SIGNAL) {
            vm_set_error(vm, instruction, "logical operation requires signal operands");
            return false;
        }
        bool a_truthy = aphelion_value_truthy(left);
        bool b_truthy = aphelion_value_truthy(right);
        *result = aphelion_value_signal(opcode == APHELION_OP_AND
                                          ? a_truthy && b_truthy
                                          : a_truthy || b_truthy);
        return true;
    }
    if (opcode == APHELION_OP_ADD && left.type == APHELION_VALUE_TEXT &&
        right.type == APHELION_VALUE_TEXT) {
        size_t left_length = left.as.text != NULL ? left.as.text->length : 0u;
        size_t right_length = right.as.text != NULL ? right.as.text->length : 0u;
        if (left_length >= SIZE_MAX - right_length) {
            vm_set_error(vm, instruction, "text concatenation is too large");
            return false;
        }
        *result = concat_text(left, right);
        if (result->type == APHELION_VALUE_HOLLOW) {
            vm_set_error(vm, instruction, "out of memory concatenating text");
            return false;
        }
        return true;
    }
    if (!numeric(left, &a, &a_whole) || !numeric(right, &b, &b_whole)) {
        vm_set_error(vm, instruction, "operator %s requires numeric values",
                     aphelion_opcode_name(opcode));
        return false;
    }
    switch (opcode) {
    case APHELION_OP_ADD:
        if (a_whole && b_whole) {
            int64_t x = left.as.whole, y = right.as.whole;
            if ((y > 0 && x > INT64_MAX - y) ||
                (y < 0 && x < INT64_MIN - y)) {
                vm_set_error(vm, instruction, "whole arithmetic overflow");
                return false;
            }
            *result = aphelion_value_whole(left.as.whole + right.as.whole);
        } else {
            *result = aphelion_value_decimal(a + b);
            if (!isfinite(result->as.decimal)) {
                vm_set_error(vm, instruction, "decimal arithmetic overflow");
                return false;
            }
        }
        return true;
    case APHELION_OP_SUBTRACT:
        if (a_whole && b_whole) {
            int64_t x = left.as.whole, y = right.as.whole;
            if ((y > 0 && x < INT64_MIN + y) ||
                (y < 0 && x > INT64_MAX + y)) {
                vm_set_error(vm, instruction, "whole arithmetic overflow");
                return false;
            }
            *result = aphelion_value_whole(left.as.whole - right.as.whole);
        } else {
            *result = aphelion_value_decimal(a - b);
            if (!isfinite(result->as.decimal)) {
                vm_set_error(vm, instruction, "decimal arithmetic overflow");
                return false;
            }
        }
        return true;
    case APHELION_OP_MULTIPLY:
        if (a_whole && b_whole) {
            int64_t x = left.as.whole, y = right.as.whole;
            if ((x > 0 && ((y > 0 && x > INT64_MAX / y) ||
                           (y < 0 && y < INT64_MIN / x))) ||
                (x < 0 && ((y > 0 && x < INT64_MIN / y) ||
                           (y < 0 && x < INT64_MAX / y)))) {
                vm_set_error(vm, instruction, "whole arithmetic overflow");
                return false;
            }
            *result = aphelion_value_whole(left.as.whole * right.as.whole);
        } else {
            *result = aphelion_value_decimal(a * b);
            if (!isfinite(result->as.decimal)) {
                vm_set_error(vm, instruction, "decimal arithmetic overflow");
                return false;
            }
        }
        return true;
    case APHELION_OP_DIVIDE:
        if (b == 0.0) {
            vm_set_error(vm, instruction, "division by zero");
            return false;
        }
        if (a_whole && b_whole) {
            if (left.as.whole == INT64_MIN && right.as.whole == -1) {
                vm_set_error(vm, instruction, "whole arithmetic overflow");
                return false;
            }
            *result = aphelion_value_whole(left.as.whole / right.as.whole);
        } else {
            *result = aphelion_value_decimal(a / b);
            if (!isfinite(result->as.decimal)) {
                vm_set_error(vm, instruction, "decimal arithmetic overflow");
                return false;
            }
        }
        return true;
    case APHELION_OP_REMAINDER:
        if (!a_whole || !b_whole) {
            vm_set_error(vm, instruction, "remainder requires whole operands");
            return false;
        }
        if (b == 0.0) {
            vm_set_error(vm, instruction, "remainder by zero");
            return false;
        }
        if (a_whole && b_whole && left.as.whole == INT64_MIN &&
            right.as.whole == -1) {
            vm_set_error(vm, instruction, "whole arithmetic overflow");
            return false;
        }
        *result = aphelion_value_whole(left.as.whole % right.as.whole);
        return true;
    default:
        vm_set_error(vm, instruction, "unsupported binary operation");
        return false;
    }
}

static bool vm_validate_target(AphelionVM *vm, int32_t target,
                               size_t code_count,
                               const AphelionInstruction *instruction) {
    if (target < 0 || (size_t)target >= code_count) {
        vm_set_error(vm, instruction, "jump target is outside the function");
        return false;
    }
    return true;
}

static bool vm_call(AphelionVM *vm, int32_t argument_count,
                    const AphelionInstruction *instruction) {
    size_t argc;
    size_t callee_position;
    AphelionValue callee;
    uint32_t function_index;
    const AphelionFunction *function;
    size_t i;
    AphelionCallFrame *frame;
    if (argument_count < 0) {
        vm_set_error(vm, instruction, "negative call argument count");
        return false;
    }
    argc = (size_t)argument_count;
    if (vm_operand_count(vm) < argc + 1u) {
        vm_set_error(vm, instruction, "operand stack underflow in call");
        return false;
    }
    callee_position = vm->stack_count - argc - 1u;
    if (vm->stack[callee_position].type != APHELION_VALUE_FUNCTION) {
        vm_set_error(vm, instruction, "attempted to call a %s value",
                     aphelion_value_type_name(vm->stack[callee_position].type));
        return false;
    }
    function_index = vm->stack[callee_position].as.function;
    if (function_index >= vm->bytecode->function_count) {
        vm_set_error(vm, instruction, "function index is invalid");
        return false;
    }
    function = &vm->bytecode->functions[function_index];
    if (function->arity != argc) {
        vm_set_error(vm, instruction, "function '%s' expects %" PRIu32
                     " argument%s, got %zu", function->name != NULL ? function->name : "<function>",
                     function->arity, function->arity == 1u ? "" : "s", argc);
        return false;
    }
    if (function->local_count < argc) {
        vm_set_error(vm, instruction, "function local metadata is invalid");
        return false;
    }
    if (vm->frame_count + 1u > vm->call_limit ||
        !vm_grow_frames(vm, vm->frame_count + 1u)) {
        vm_set_error(vm, instruction, "call-depth limit exceeded");
        return false;
    }
    if (function->local_count > SIZE_MAX - callee_position ||
        !vm_grow_stack(vm, callee_position + function->local_count)) {
        vm_set_error(vm, instruction, "operand stack limit exceeded in call");
        return false;
    }
    callee = aphelion_value_move(&vm->stack[callee_position]);
    aphelion_value_destroy(&callee);
    for (i = 0u; i < argc; ++i) {
        vm->stack[callee_position + i] =
            aphelion_value_move(&vm->stack[callee_position + i + 1u]);
    }
    for (i = argc; i < function->local_count; ++i) {
        vm->stack[callee_position + i] = aphelion_value_hollow();
        vm->stack[callee_position + i].type = APHELION_VALUE_UNINITIALIZED;
    }
    vm->stack_count = callee_position + function->local_count;
    frame = &vm->frames[vm->frame_count++];
    frame->function_index = function_index;
    frame->instruction = 0u;
    frame->base = callee_position;
    frame->source_name = vm->bytecode->source_name;
    return true;
}

static bool vm_return(AphelionVM *vm, const AphelionInstruction *instruction) {
    AphelionCallFrame *frame;
    AphelionValue result;
    size_t base;
    if (vm->frame_count == 0u) {
        vm_set_error(vm, instruction, "return without a call frame");
        return false;
    }
    frame = &vm->frames[vm->frame_count - 1u];
    base = frame->base;
    if (!vm_pop_move(vm, &result, instruction)) return false;
    while (vm->stack_count > base) {
        vm->stack_count -= 1u;
        aphelion_value_destroy(&vm->stack[vm->stack_count]);
    }
    vm->frame_count -= 1u;
    if (vm->frame_count == 0u) {
        aphelion_value_destroy(&result);
        vm->halted = true;
        return true;
    }
    return vm_push_move(vm, &result, instruction);
}

bool aphelion_vm_run(AphelionVM *vm, const AphelionBytecode *bytecode) {
    uint64_t steps = 0u;
    char validation_error[256];
    if (vm == NULL) {
        return false;
    }
    vm_clear_stack(vm);
    vm_clear_globals(vm);
    vm->frame_count = 0u;
    vm->failed = false;
    vm->halted = false;
    memset(&vm->error, 0, sizeof(vm->error));
    vm->bytecode = bytecode;
    if (!aphelion_bytecode_validate(bytecode, validation_error, sizeof(validation_error))) {
        vm_set_error(vm, NULL, "%s", validation_error);
        return false;
    }
    vm->global_count = bytecode->global_count;
    if (vm->global_count != 0u) {
        size_t i;
        vm->globals = (AphelionValue *)calloc(vm->global_count,
                                              sizeof(*vm->globals));
        if (vm->globals == NULL) {
            vm_set_error(vm, NULL, "out of memory allocating globals");
            return false;
        }
        for (i = 0u; i < vm->global_count; ++i) {
            vm->globals[i] = aphelion_value_hollow();
            vm->globals[i].type = APHELION_VALUE_UNINITIALIZED;
        }
    }
    if (!vm_grow_frames(vm, 1u)) {
        vm_set_error(vm, NULL, "call-depth limit is zero");
        return false;
    }
    {
        const AphelionFunction *entry = &bytecode->functions[bytecode->entry_function];
        size_t i;
        vm->frames[0].function_index = bytecode->entry_function;
        vm->frames[0].instruction = 0u;
        vm->frames[0].base = 0u;
        vm->frames[0].source_name = bytecode->source_name;
        vm->frame_count = 1u;
        if (entry->local_count > vm->stack_limit ||
            !vm_grow_stack(vm, entry->local_count)) {
            vm_set_error(vm, NULL, "entry function exceeds operand stack limit");
            return false;
        }
        for (i = 0u; i < entry->local_count; ++i) {
            vm->stack[vm->stack_count] = aphelion_value_hollow();
            vm->stack[vm->stack_count++].type = APHELION_VALUE_UNINITIALIZED;
        }
    }
    while (!vm->failed && !vm->halted && vm->frame_count != 0u) {
        AphelionCallFrame *frame = &vm->frames[vm->frame_count - 1u];
        const AphelionFunction *function = &bytecode->functions[frame->function_index];
        AphelionInstruction instruction;
        AphelionValue left;
        AphelionValue right;
        AphelionValue result;
        if (frame->instruction >= function->code_count) {
            vm_set_error(vm, NULL, "instruction pointer ran past function '%s'",
                         function->name != NULL ? function->name : "<function>");
            break;
        }
        instruction = function->code[frame->instruction++];
        if (steps++ >= vm->instruction_limit) {
            vm_set_error(vm, &instruction, "VM instruction limit exceeded");
            break;
        }
        switch ((AphelionOpcode)instruction.opcode) {
        case APHELION_OP_HALT:
            vm->halted = true;
            break;
        case APHELION_OP_SOURCE:
            /* Validation guarantees a nonempty, NUL-free text constant. The
             * bytecode owns it, so error locations survive frame teardown. */
            frame->source_name = bytecode->constants[instruction.operand].as.text->bytes;
            break;
        case APHELION_OP_CONSTANT:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= bytecode->constant_count) {
                vm_set_error(vm, &instruction, "constant index is invalid");
                break;
            }
            (void)vm_push_copy(vm, bytecode->constants[instruction.operand], &instruction);
            break;
        case APHELION_OP_HOLLOW:
            result = aphelion_value_hollow();
            (void)vm_push_move(vm, &result, &instruction);
            break;
        case APHELION_OP_SILENT:
            result = aphelion_value_silent();
            (void)vm_push_move(vm, &result, &instruction);
            break;
        case APHELION_OP_LIST:
            (void)vm_list(vm, &instruction);
            break;
        case APHELION_OP_INDEX:
            (void)vm_index(vm, &instruction);
            break;
        case APHELION_OP_POP:
            if (vm_pop_move(vm, &result, &instruction)) aphelion_value_destroy(&result);
            break;
        case APHELION_OP_DUP:
            if (vm_operand_count(vm) == 0u) {
                vm_set_error(vm, &instruction, "operand stack underflow");
            } else {
                (void)vm_push_copy(vm, vm->stack[vm->stack_count - 1u], &instruction);
            }
            break;
        case APHELION_OP_LOAD_LOCAL:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= function->local_count ||
                frame->base + (size_t)instruction.operand >= vm->stack_count) {
                vm_set_error(vm, &instruction, "local slot is invalid");
            } else {
                if (vm->stack[frame->base + (size_t)instruction.operand].type ==
                    APHELION_VALUE_UNINITIALIZED) {
                    vm_set_error(vm, &instruction, "local read before initialization");
                    break;
                }
                (void)vm_push_copy(vm,
                                   vm->stack[frame->base + (size_t)instruction.operand],
                                   &instruction);
            }
            break;
        case APHELION_OP_STORE_LOCAL:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= function->local_count ||
                frame->base + (size_t)instruction.operand >= vm->stack_count ||
                !vm_pop_move(vm, &result, &instruction)) {
                if (!vm->failed) vm_set_error(vm, &instruction, "local slot is invalid");
            } else {
                AphelionValue *slot = &vm->stack[frame->base + (size_t)instruction.operand];
                aphelion_value_destroy(slot);
                *slot = result;
            }
            break;
        case APHELION_OP_LOAD_GLOBAL:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= vm->global_count) {
                vm_set_error(vm, &instruction, "global slot is invalid");
            } else {
                if (vm->globals[instruction.operand].type == APHELION_VALUE_UNINITIALIZED) {
                    vm_set_error(vm, &instruction, "global '%s' read before initialization",
                                 bytecode->globals[instruction.operand]);
                    break;
                }
                (void)vm_push_copy(vm, vm->globals[instruction.operand], &instruction);
            }
            break;
        case APHELION_OP_STORE_GLOBAL:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= vm->global_count ||
                !vm_pop_move(vm, &result, &instruction)) {
                if (!vm->failed) vm_set_error(vm, &instruction, "global slot is invalid");
            } else {
                aphelion_value_destroy(&vm->globals[instruction.operand]);
                vm->globals[instruction.operand] = result;
            }
            break;
        case APHELION_OP_MAKE_FUNCTION:
            if (instruction.operand < 0 ||
                (size_t)instruction.operand >= bytecode->function_count) {
                vm_set_error(vm, &instruction, "function index is invalid");
            } else {
                result = aphelion_value_function((uint32_t)instruction.operand);
                (void)vm_push_move(vm, &result, &instruction);
            }
            break;
        case APHELION_OP_ADD:
        case APHELION_OP_SUBTRACT:
        case APHELION_OP_MULTIPLY:
        case APHELION_OP_DIVIDE:
        case APHELION_OP_REMAINDER:
        case APHELION_OP_EQUAL:
        case APHELION_OP_NOT_EQUAL:
        case APHELION_OP_LESS:
        case APHELION_OP_LESS_EQUAL:
        case APHELION_OP_GREATER:
        case APHELION_OP_GREATER_EQUAL:
        case APHELION_OP_AND:
        case APHELION_OP_OR:
            if (!vm_pop_move(vm, &right, &instruction)) {
                break;
            }
            if (!vm_pop_move(vm, &left, &instruction)) {
                aphelion_value_destroy(&right);
                if (!vm->failed) vm_set_error(vm, &instruction, "operand stack underflow");
                break;
            }
            if (vm_binary(vm, (AphelionOpcode)instruction.opcode, left, right,
                          &result, &instruction)) {
                (void)vm_push_move(vm, &result, &instruction);
            }
            aphelion_value_destroy(&left);
            aphelion_value_destroy(&right);
            break;
        case APHELION_OP_NEGATE:
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (left.type == APHELION_VALUE_WHOLE) {
                if (left.as.whole == INT64_MIN) {
                    vm_set_error(vm, &instruction, "whole arithmetic overflow");
                } else {
                    result = aphelion_value_whole(-left.as.whole);
                    (void)vm_push_move(vm, &result, &instruction);
                }
            } else if (left.type == APHELION_VALUE_DECIMAL) {
                result = aphelion_value_decimal(-left.as.decimal);
                (void)vm_push_move(vm, &result, &instruction);
            } else {
                vm_set_error(vm, &instruction, "negation requires a numeric value");
            }
            aphelion_value_destroy(&left);
            break;
        case APHELION_OP_NOT:
            if (vm_pop_move(vm, &left, &instruction)) {
                if (left.type != APHELION_VALUE_SIGNAL) {
                    vm_set_error(vm, &instruction, "not requires signal operand");
                    aphelion_value_destroy(&left);
                    break;
                }
                result = aphelion_value_signal(!aphelion_value_truthy(left));
                aphelion_value_destroy(&left);
                (void)vm_push_move(vm, &result, &instruction);
            }
            break;
        case APHELION_OP_JUMP:
            if (vm_validate_target(vm, instruction.operand, function->code_count,
                                   &instruction)) {
                frame->instruction = (size_t)instruction.operand;
            }
            break;
        case APHELION_OP_JUMP_IF_FALSE:
        case APHELION_OP_JUMP_IF_TRUE:
            if (vm_operand_count(vm) == 0u) {
                vm_set_error(vm, &instruction, "operand stack underflow in conditional jump");
            } else if (vm->stack[vm->stack_count - 1u].type != APHELION_VALUE_SIGNAL) {
                vm_set_error(vm, &instruction, "conditional jump requires signal");
            } else if (vm_validate_target(vm, instruction.operand, function->code_count,
                                          &instruction) &&
                       ((instruction.opcode == APHELION_OP_JUMP_IF_FALSE &&
                         !aphelion_value_truthy(vm->stack[vm->stack_count - 1u])) ||
                        (instruction.opcode == APHELION_OP_JUMP_IF_TRUE &&
                         aphelion_value_truthy(vm->stack[vm->stack_count - 1u])))) {
                frame->instruction = (size_t)instruction.operand;
            }
            break;
        case APHELION_OP_CALL:
            (void)vm_call(vm, instruction.operand, &instruction);
            break;
        case APHELION_OP_RETURN:
            (void)vm_return(vm, &instruction);
            break;
        case APHELION_OP_REVEAL:
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (!aphelion_value_write(vm->output != NULL ? vm->output : stdout, left) ||
                fputc('\n', vm->output != NULL ? vm->output : stdout) == EOF) {
                vm_set_error(vm, &instruction, "output failed");
            }
            aphelion_value_destroy(&left);
            if (!vm->failed) {
                result = aphelion_value_silent();
                (void)vm_push_move(vm, &result, &instruction);
            }
            break;
        case APHELION_OP_RECEIVE:
            if (vm_read_line(vm, &instruction, &result)) {
                (void)vm_push_move(vm, &result, &instruction);
            }
            break;
        case APHELION_OP_TO_WHOLE:
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (vm_to_whole(vm, left, &instruction, &result)) {
                (void)vm_push_move(vm, &result, &instruction);
            }
            aphelion_value_destroy(&left);
            break;
        case APHELION_OP_TO_DECIMAL:
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (vm_to_decimal(vm, left, &instruction, &result)) {
                (void)vm_push_move(vm, &result, &instruction);
            }
            aphelion_value_destroy(&left);
            break;
        case APHELION_OP_LENGTH: {
            size_t length;
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (left.type == APHELION_VALUE_TEXT && left.as.text != NULL) {
                length = left.as.text->length;
            } else if (left.type == APHELION_VALUE_LIST && left.as.list != NULL) {
                length = left.as.list->length;
            } else {
                vm_set_error(vm, &instruction, "length requires text or list, got %s",
                             aphelion_value_type_name(left.type));
                aphelion_value_destroy(&left);
                break;
            }
#if SIZE_MAX > INT64_MAX
            if (length > (size_t)INT64_MAX) {
                vm_set_error(vm, &instruction, "length is out of whole range");
            } else
#endif
            {
                result = aphelion_value_whole((int64_t)length);
                (void)vm_push_move(vm, &result, &instruction);
            }
            aphelion_value_destroy(&left);
            break;
        }
        case APHELION_OP_KIND:
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (left.type < APHELION_VALUE_HOLLOW ||
                left.type > APHELION_VALUE_LIST ||
                left.type == APHELION_VALUE_UNINITIALIZED) {
                vm_set_error(vm, &instruction, "kind requires an initialized value");
            } else {
                result = aphelion_value_text(aphelion_value_type_name(left.type));
                if (result.type != APHELION_VALUE_TEXT) {
                    vm_set_error(vm, &instruction, "out of memory creating kind text");
                } else {
                    (void)vm_push_move(vm, &result, &instruction);
                }
            }
            aphelion_value_destroy(&left);
            break;
        case APHELION_OP_ASSERT:
            (void)vm_assert(vm, &instruction);
            break;
        case APHELION_OP_TO_TEXT: {
            char *text;
            if (!vm_pop_move(vm, &left, &instruction)) break;
            if (left.type == APHELION_VALUE_TEXT) {
                (void)vm_push_move(vm, &left, &instruction);
                break;
            }
            text = aphelion_value_to_string(left);
            aphelion_value_destroy(&left);
            if (text == NULL) {
                vm_set_error(vm, &instruction, "out of memory converting value to text");
                break;
            }
            result = aphelion_value_text(text);
            free(text);
            if (result.type != APHELION_VALUE_TEXT) {
                vm_set_error(vm, &instruction, "out of memory creating converted text");
                break;
            }
            (void)vm_push_move(vm, &result, &instruction);
            break;
        }
        case APHELION_OP_OUTPUT:
            vm_set_error(vm, &instruction, "unresolved output opcode");
            break;
        case APHELION_OP_BREAK:
        case APHELION_OP_SKIP:
            vm_set_error(vm, &instruction, "unresolved control-flow opcode");
            break;
        case APHELION_OP_NOP:
            break;
        default:
            vm_set_error(vm, &instruction, "unknown opcode %u", instruction.opcode);
            break;
        }
    }
    if (vm->failed) {
        vm_clear_stack(vm);
        vm->frame_count = 0u;
        vm_clear_globals(vm);
        return false;
    }
    vm_clear_stack(vm);
    vm->frame_count = 0u;
    vm_clear_globals(vm);
    return true;
}
