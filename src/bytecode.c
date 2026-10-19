#include "aphelion/bytecode.h"
#include "aphelion/file.h"

#include <inttypes.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, ".apb requires 8-bit bytes");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "Aphelion decimal requires IEEE-754 binary64");

static char *bc_strdup(const char *text) {
    size_t length;
    char *copy;
    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length + 1u);
    }
    return copy;
}

static bool grow_array(void **data, size_t *capacity, size_t count,
                       size_t element_size) {
    size_t next;
    void *grown;
    if (count <= *capacity) {
        return true;
    }
    next = *capacity == 0u ? 8u : *capacity;
    while (next < count) {
        if (next > SIZE_MAX / 2u) {
            next = count;
            break;
        }
        next *= 2u;
    }
    if (element_size != 0u && next > SIZE_MAX / element_size) {
        return false;
    }
    grown = realloc(*data, next * element_size);
    if (grown == NULL) {
        return false;
    }
    *data = grown;
    *capacity = next;
    return true;
}

void aphelion_bytecode_init(AphelionBytecode *bytecode) {
    if (bytecode != NULL) {
        memset(bytecode, 0, sizeof(*bytecode));
    }
}

void aphelion_bytecode_destroy(AphelionBytecode *bytecode) {
    size_t i;
    if (bytecode == NULL) {
        return;
    }
    for (i = 0u; i < bytecode->constant_count; ++i) {
        aphelion_value_destroy(&bytecode->constants[i]);
    }
    free(bytecode->constants);
    for (i = 0u; i < bytecode->function_count; ++i) {
        free(bytecode->functions[i].name);
        free(bytecode->functions[i].code);
    }
    free(bytecode->functions);
    for (i = 0u; i < bytecode->global_count; ++i) {
        free(bytecode->globals[i]);
    }
    free(bytecode->globals);
    free(bytecode->source_name);
    memset(bytecode, 0, sizeof(*bytecode));
}

int aphelion_bytecode_add_constant(AphelionBytecode *bytecode,
                                   AphelionValue value) {
    if (bytecode == NULL || bytecode->constant_count >= INT32_MAX ||
        !grow_array((void **)&bytecode->constants,
                    &bytecode->constant_capacity,
                    bytecode->constant_count + 1u,
                    sizeof(*bytecode->constants))) {
        return -1;
    }
    bytecode->constants[bytecode->constant_count] = aphelion_value_copy(value);
    return (int)bytecode->constant_count++;
}

int aphelion_bytecode_add_global(AphelionBytecode *bytecode,
                                 const char *name) {
    char *copy;
    size_t i;
    if (bytecode == NULL || name == NULL || bytecode->global_count >= INT32_MAX) {
        return -1;
    }
    for (i = 0u; i < bytecode->global_count; ++i) {
        if (strcmp(bytecode->globals[i], name) == 0) {
            return (int)i;
        }
    }
    if (!grow_array((void **)&bytecode->globals,
                    &bytecode->global_capacity,
                    bytecode->global_count + 1u,
                    sizeof(*bytecode->globals))) {
        return -1;
    }
    copy = bc_strdup(name);
    if (copy == NULL) {
        return -1;
    }
    bytecode->globals[bytecode->global_count] = copy;
    return (int)bytecode->global_count++;
}

int aphelion_bytecode_add_function(AphelionBytecode *bytecode,
                                   const char *name,
                                   uint32_t arity,
                                   uint32_t local_count) {
    AphelionFunction *function;
    if (bytecode == NULL || name == NULL || bytecode->function_count >= INT32_MAX ||
        !grow_array((void **)&bytecode->functions,
                    &bytecode->function_capacity,
                    bytecode->function_count + 1u,
                    sizeof(*bytecode->functions))) {
        return -1;
    }
    function = &bytecode->functions[bytecode->function_count];
    memset(function, 0, sizeof(*function));
    function->name = bc_strdup(name);
    if (function->name == NULL) {
        return -1;
    }
    function->arity = arity;
    function->local_count = local_count;
    return (int)bytecode->function_count++;
}

bool aphelion_bytecode_set_source_name(AphelionBytecode *bytecode,
                                       const char *name) {
    char *copy = NULL;
    if (bytecode == NULL) {
        return false;
    }
    if (name != NULL) {
        copy = bc_strdup(name);
        if (copy == NULL) {
            return false;
        }
    }
    free(bytecode->source_name);
    bytecode->source_name = copy;
    return true;
}

int aphelion_function_emit(AphelionFunction *function,
                           AphelionOpcode opcode,
                           int32_t operand,
                           int32_t operand2,
                           uint32_t line,
                           uint32_t column) {
    AphelionInstruction *instruction;
    if (function == NULL || function->code_count >= INT32_MAX ||
        !grow_array((void **)&function->code,
                    &function->code_capacity,
                    function->code_count + 1u,
                    sizeof(*function->code))) {
        return -1;
    }
    instruction = &function->code[function->code_count];
    instruction->opcode = (uint8_t)opcode;
    instruction->operand = operand;
    instruction->operand2 = operand2;
    instruction->line = line;
    instruction->column = column;
    return (int)function->code_count++;
}

bool aphelion_function_patch_operand(AphelionFunction *function,
                                     size_t instruction,
                                     int32_t operand) {
    if (function == NULL || instruction >= function->code_count) {
        return false;
    }
    function->code[instruction].operand = operand;
    return true;
}

const char *aphelion_opcode_name(AphelionOpcode opcode) {
    switch (opcode) {
    case APHELION_OP_HALT: return "HALT";
    case APHELION_OP_CONSTANT: return "CONSTANT";
    case APHELION_OP_HOLLOW: return "HOLLOW";
    case APHELION_OP_SILENT: return "SILENT";
    case APHELION_OP_POP: return "POP";
    case APHELION_OP_DUP: return "DUP";
    case APHELION_OP_LOAD_LOCAL: return "LOAD_LOCAL";
    case APHELION_OP_STORE_LOCAL: return "STORE_LOCAL";
    case APHELION_OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
    case APHELION_OP_STORE_GLOBAL: return "STORE_GLOBAL";
    case APHELION_OP_MAKE_FUNCTION: return "MAKE_FUNCTION";
    case APHELION_OP_ADD: return "ADD";
    case APHELION_OP_SUBTRACT: return "SUBTRACT";
    case APHELION_OP_MULTIPLY: return "MULTIPLY";
    case APHELION_OP_DIVIDE: return "DIVIDE";
    case APHELION_OP_REMAINDER: return "REMAINDER";
    case APHELION_OP_NEGATE: return "NEGATE";
    case APHELION_OP_NOT: return "NOT";
    case APHELION_OP_EQUAL: return "EQUAL";
    case APHELION_OP_NOT_EQUAL: return "NOT_EQUAL";
    case APHELION_OP_LESS: return "LESS";
    case APHELION_OP_LESS_EQUAL: return "LESS_EQUAL";
    case APHELION_OP_GREATER: return "GREATER";
    case APHELION_OP_GREATER_EQUAL: return "GREATER_EQUAL";
    case APHELION_OP_AND: return "AND";
    case APHELION_OP_OR: return "OR";
    case APHELION_OP_JUMP: return "JUMP";
    case APHELION_OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case APHELION_OP_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
    case APHELION_OP_CALL: return "CALL";
    case APHELION_OP_RETURN: return "RETURN";
    case APHELION_OP_REVEAL: return "REVEAL";
    case APHELION_OP_TO_TEXT: return "TO_TEXT";
    case APHELION_OP_RECEIVE: return "RECEIVE";
    case APHELION_OP_TO_WHOLE: return "TO_WHOLE";
    case APHELION_OP_TO_DECIMAL: return "TO_DECIMAL";
    case APHELION_OP_LENGTH: return "LENGTH";
    case APHELION_OP_KIND: return "KIND";
    case APHELION_OP_ASSERT: return "ASSERT";
    case APHELION_OP_LIST: return "LIST";
    case APHELION_OP_INDEX: return "INDEX";
    case APHELION_OP_SOURCE: return "SOURCE";
    case APHELION_OP_OUTPUT: return "OUTPUT";
    case APHELION_OP_BREAK: return "BREAK";
    case APHELION_OP_SKIP: return "SKIP";
    case APHELION_OP_NOP: return "NOP";
    default: return "<INVALID>";
    }
}

bool aphelion_bytecode_disassemble(const AphelionBytecode *bytecode,
                                   FILE *out) {
    size_t i;
    size_t j;
    if (bytecode == NULL || out == NULL) {
        return false;
    }
    (void)fprintf(out, "source: %s\n", bytecode->source_name != NULL
                                      ? bytecode->source_name : "<bytecode>");
    (void)fprintf(out, "constants (%zu):\n", bytecode->constant_count);
    for (i = 0u; i < bytecode->constant_count; ++i) {
        char *text = aphelion_value_to_string(bytecode->constants[i]);
        if (text == NULL) return false;
        (void)fprintf(out, "  [%zu] %s\n", i, text);
        free(text);
    }
    (void)fprintf(out, "globals (%zu):\n", bytecode->global_count);
    for (i = 0u; i < bytecode->global_count; ++i) {
        (void)fprintf(out, "  [%zu] %s\n", i, bytecode->globals[i]);
    }
    for (i = 0u; i < bytecode->function_count; ++i) {
        const AphelionFunction *function = &bytecode->functions[i];
        (void)fprintf(out, "function %zu %s (arity=%" PRIu32 ", locals=%" PRIu32 ")%s\n",
                      i, function->name != NULL ? function->name : "<anonymous>",
                      function->arity, function->local_count,
                      i == bytecode->entry_function ? " [entry]" : "");
        for (j = 0u; j < function->code_count; ++j) {
            const AphelionInstruction *instruction = &function->code[j];
            (void)fprintf(out, "  %04zu  %4" PRIu32 ":%" PRIu32 "  %-16s %" PRId32,
                          j, instruction->line, instruction->column,
                          aphelion_opcode_name((AphelionOpcode)instruction->opcode),
                          instruction->operand);
            if (instruction->operand2 != 0) {
                (void)fprintf(out, " %" PRId32, instruction->operand2);
            }
            (void)fputc('\n', out);
        }
    }
    return !ferror(out);
}

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (error == NULL || error_size == 0u) {
        return;
    }
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool write_bytes(FILE *file, const void *data, size_t size) {
    return size == 0u || fwrite(data, 1u, size, file) == size;
}

static bool write_u8(FILE *file, uint8_t value) {
    return write_bytes(file, &value, sizeof(value));
}

static bool write_u32(FILE *file, uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16u) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24u) & 0xffu);
    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_i32(FILE *file, int32_t value) {
    return write_u32(file, (uint32_t)value);
}

static bool write_u64(FILE *file, uint64_t value) {
    uint8_t bytes[8];
    size_t i;
    for (i = 0u; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    }
    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_string(FILE *file, const char *text) {
    size_t length = text != NULL ? strlen(text) : 0u;
    if (length > UINT32_MAX) {
        return false;
    }
    return write_u32(file, (uint32_t)length) && write_bytes(file, text, length);
}

static bool write_value(FILE *file, AphelionValue value) {
    uint64_t bits;
    if (!write_u8(file, (uint8_t)value.type)) {
        return false;
    }
    switch (value.type) {
    case APHELION_VALUE_WHOLE:
        return write_u64(file, (uint64_t)value.as.whole);
    case APHELION_VALUE_DECIMAL:
        memcpy(&bits, &value.as.decimal, sizeof(bits));
        return write_u64(file, bits);
    case APHELION_VALUE_SIGNAL:
        return write_u8(file, value.as.signal ? 1u : 0u);
    case APHELION_VALUE_TEXT:
        if (value.as.text == NULL || value.as.text->length > UINT32_MAX) return false;
        return write_u32(file, (uint32_t)value.as.text->length) &&
               write_bytes(file, value.as.text->bytes, value.as.text->length);
    case APHELION_VALUE_GLYPH:
    case APHELION_VALUE_FUNCTION:
        return write_u32(file, value.type == APHELION_VALUE_GLYPH
                                    ? value.as.glyph : value.as.function);
    case APHELION_VALUE_HOLLOW:
    case APHELION_VALUE_SILENT:
        return true;
    default:
        return false;
    }
}

bool aphelion_bytecode_write_file(const AphelionBytecode *bytecode,
                                  const char *path,
                                  char *error,
                                  size_t error_size) {
    FILE *file;
    size_t i;
    size_t j;
    if (bytecode == NULL || path == NULL) {
        set_error(error, error_size, "invalid bytecode output arguments");
        return false;
    }
    if (bytecode->constant_count > UINT32_MAX ||
        bytecode->function_count > UINT32_MAX ||
        bytecode->global_count > UINT32_MAX ||
        bytecode->entry_function >= bytecode->function_count) {
        set_error(error, error_size, "bytecode contains too many sections");
        return false;
    }
    for (i = 0u; i < bytecode->constant_count; ++i) {
        if (bytecode->constants[i].type < APHELION_VALUE_HOLLOW ||
            bytecode->constants[i].type > APHELION_VALUE_SILENT) {
            set_error(error, error_size, "invalid serialized constant type");
            return false;
        }
    }
    file = aphelion_file_open(path, "wb");
    if (file == NULL) {
        set_error(error, error_size, "cannot open '%s' for writing", path);
        return false;
    }
    if (!write_bytes(file, APHELION_BYTECODE_MAGIC, 4u) ||
        !write_u32(file, APHELION_BYTECODE_VERSION) ||
        !write_u32(file, (uint32_t)bytecode->entry_function) ||
        !write_u32(file, (uint32_t)bytecode->constant_count) ||
        !write_u32(file, (uint32_t)bytecode->function_count) ||
        !write_u32(file, (uint32_t)bytecode->global_count) ||
        !write_string(file, bytecode->source_name)) {
        set_error(error, error_size, "write failed for '%s'", path);
        (void)fclose(file);
        return false;
    }
    for (i = 0u; i < bytecode->constant_count; ++i) {
        if (!write_value(file, bytecode->constants[i])) {
            set_error(error, error_size, "write failed for '%s'", path);
            (void)fclose(file);
            return false;
        }
    }
    for (i = 0u; i < bytecode->global_count; ++i) {
        if (!write_string(file, bytecode->globals[i])) {
            set_error(error, error_size, "write failed for '%s'", path);
            (void)fclose(file);
            return false;
        }
    }
    for (i = 0u; i < bytecode->function_count; ++i) {
        const AphelionFunction *function = &bytecode->functions[i];
        if (function->code_count > UINT32_MAX ||
            !write_string(file, function->name) ||
            !write_u32(file, function->arity) ||
            !write_u32(file, function->local_count) ||
            !write_u32(file, (uint32_t)function->code_count)) {
            set_error(error, error_size, "write failed for '%s'", path);
            (void)fclose(file);
            return false;
        }
        for (j = 0u; j < function->code_count; ++j) {
            const AphelionInstruction *instruction = &function->code[j];
            if (!write_u8(file, instruction->opcode) ||
                !write_i32(file, instruction->operand) ||
                !write_i32(file, instruction->operand2) ||
                !write_u32(file, instruction->line) ||
                !write_u32(file, instruction->column)) {
                set_error(error, error_size, "write failed for '%s'", path);
                (void)fclose(file);
                return false;
            }
        }
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "close failed for '%s'", path);
        return false;
    }
    return true;
}

static bool read_bytes(FILE *file, void *data, size_t size) {
    return size == 0u || fread(data, 1u, size, file) == size;
}

static bool read_u8(FILE *file, uint8_t *value) {
    return read_bytes(file, value, sizeof(*value));
}

static bool read_u32(FILE *file, uint32_t *value) {
    uint8_t bytes[4];
    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8u) |
             ((uint32_t)bytes[2] << 16u) |
             ((uint32_t)bytes[3] << 24u);
    return true;
}

static bool read_i32(FILE *file, int32_t *value) {
    uint32_t raw;
    if (!read_u32(file, &raw)) {
        return false;
    }
    *value = raw <= INT32_MAX ? (int32_t)raw : -1 - (int32_t)(UINT32_MAX - raw);
    return true;
}

static bool read_u64(FILE *file, uint64_t *value) {
    uint8_t bytes[8];
    size_t i;
    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return false;
    }
    *value = 0u;
    for (i = 0u; i < sizeof(bytes); ++i) {
        *value |= (uint64_t)bytes[i] << (i * 8u);
    }
    return true;
}

static bool file_has_bytes(FILE *file, size_t count) {
    long position = ftell(file), end;
    if (position < 0L || fseek(file, 0L, SEEK_END) != 0) return false;
    end = ftell(file);
    if (fseek(file, position, SEEK_SET) != 0 || end < position) return false;
    return count <= (size_t)(end - position);
}

static char *read_string(FILE *file, bool null_when_empty, bool *ok,
                          size_t *length_out) {
    uint32_t length;
    char *text;
    if (!read_u32(file, &length) || length > 64u * 1024u * 1024u ||
        !file_has_bytes(file, length)) {
        *ok = false;
        return NULL;
    }
    if (length_out != NULL) *length_out = length;
    if (length == 0u && null_when_empty) {
        *ok = true;
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1u);
    if (text == NULL) {
        *ok = false;
        return NULL;
    }
    if (!read_bytes(file, text, length)) {
        free(text);
        *ok = false;
        return NULL;
    }
    text[length] = '\0';
    if (length_out == NULL && memchr(text, '\0', length) != NULL) {
        free(text);
        *ok = false;
        return NULL;
    }
    *ok = true;
    return text;
}

static bool read_value(FILE *file, AphelionValue *value) {
    uint8_t type;
    uint8_t signal;
    uint32_t raw;
    uint64_t bits;
    bool ok;
    char *text;
    size_t length;
    if (!read_u8(file, &type) || type > APHELION_VALUE_SILENT) {
        return false;
    }
    switch ((AphelionValueType)type) {
    case APHELION_VALUE_WHOLE:
        if (!read_u64(file, &bits)) return false;
        *value = aphelion_value_whole(bits <= INT64_MAX ? (int64_t)bits
                                      : -1 - (int64_t)(UINT64_MAX - bits));
        return true;
    case APHELION_VALUE_DECIMAL:
        if (!read_u64(file, &bits)) return false;
        memcpy(&value->as.decimal, &bits, sizeof(bits));
        value->type = APHELION_VALUE_DECIMAL;
        return isfinite(value->as.decimal);
    case APHELION_VALUE_SIGNAL:
        if (!read_u8(file, &signal) || signal > 1u) return false;
        *value = aphelion_value_signal(signal != 0u);
        return true;
    case APHELION_VALUE_TEXT:
        text = read_string(file, false, &ok, &length);
        if (!ok) return false;
        *value = aphelion_value_text_n(text, length);
        free(text);
        return value->type == APHELION_VALUE_TEXT;
    case APHELION_VALUE_GLYPH:
    case APHELION_VALUE_FUNCTION:
        if (!read_u32(file, &raw)) return false;
        if (type == APHELION_VALUE_GLYPH &&
            (raw > 0x10ffffu || (raw >= 0xd800u && raw <= 0xdfffu))) return false;
        *value = type == APHELION_VALUE_GLYPH
                   ? aphelion_value_glyph(raw) : aphelion_value_function(raw);
        return true;
    case APHELION_VALUE_HOLLOW:
        *value = aphelion_value_hollow();
        return true;
    case APHELION_VALUE_SILENT:
        *value = aphelion_value_silent();
        return true;
    default:
        return false;
    }
}

static bool validate_instruction(const AphelionBytecode *bytecode,
                                const AphelionFunction *function,
                                const AphelionInstruction *instruction,
                                char *error, size_t error_size) {
    AphelionOpcode opcode;
    if (instruction == NULL || function == NULL || bytecode == NULL) {
        set_error(error, error_size, "invalid instruction metadata");
        return false;
    }
    opcode = (AphelionOpcode)instruction->opcode;
    if (opcode > APHELION_OP_SOURCE ||
        opcode == APHELION_OP_OUTPUT || opcode == APHELION_OP_BREAK ||
        opcode == APHELION_OP_SKIP) {
        set_error(error, error_size, "unsupported opcode %u in function '%s'",
                  instruction->opcode,
                  function->name != NULL ? function->name : "<function>");
        return false;
    }
    switch (opcode) {
    case APHELION_OP_SOURCE:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= bytecode->constant_count) {
            set_error(error, error_size, "SOURCE opcode constant operand is out of range");
            return false;
        }
        {
            AphelionValue value = bytecode->constants[instruction->operand];
            if (value.type != APHELION_VALUE_TEXT || value.as.text == NULL ||
                value.as.text->length == 0u ||
                memchr(value.as.text->bytes, '\0', value.as.text->length) != NULL) {
                set_error(error, error_size, "SOURCE operand must reference nonempty NUL-free text");
                return false;
            }
        }
        break;
    case APHELION_OP_CONSTANT:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= bytecode->constant_count) {
            set_error(error, error_size, "constant operand is out of range");
            return false;
        }
        break;
    case APHELION_OP_LOAD_LOCAL:
    case APHELION_OP_STORE_LOCAL:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= function->local_count) {
            set_error(error, error_size, "local operand is out of range");
            return false;
        }
        break;
    case APHELION_OP_LOAD_GLOBAL:
    case APHELION_OP_STORE_GLOBAL:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= bytecode->global_count) {
            set_error(error, error_size, "global operand is out of range");
            return false;
        }
        break;
    case APHELION_OP_MAKE_FUNCTION:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= bytecode->function_count) {
            set_error(error, error_size, "function operand is out of range");
            return false;
        }
        break;
    case APHELION_OP_JUMP:
    case APHELION_OP_JUMP_IF_FALSE:
    case APHELION_OP_JUMP_IF_TRUE:
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= function->code_count) {
            set_error(error, error_size, "jump operand is out of range");
            return false;
        }
        break;
    case APHELION_OP_CALL:
        if (instruction->operand < 0) {
            set_error(error, error_size, "call argument count is negative");
            return false;
        }
        break;
    case APHELION_OP_ASSERT:
        if (instruction->operand != 1 && instruction->operand != 2) {
            set_error(error, error_size, "ASSERT argument count must be 1 or 2");
            return false;
        }
        break;
    case APHELION_OP_LIST:
        if (instruction->operand < 0 ||
            ((instruction->operand2 < APHELION_VALUE_WHOLE ||
              instruction->operand2 > APHELION_VALUE_GLYPH) &&
             !(instruction->operand == 0 &&
               instruction->operand2 == APHELION_VALUE_HOLLOW))) {
            set_error(error, error_size, "invalid LIST count or element type operand");
            return false;
        }
        break;
    case APHELION_OP_HALT:
    case APHELION_OP_HOLLOW:
    case APHELION_OP_SILENT:
    case APHELION_OP_POP:
    case APHELION_OP_DUP:
    case APHELION_OP_ADD:
    case APHELION_OP_SUBTRACT:
    case APHELION_OP_MULTIPLY:
    case APHELION_OP_DIVIDE:
    case APHELION_OP_REMAINDER:
    case APHELION_OP_NEGATE:
    case APHELION_OP_NOT:
    case APHELION_OP_EQUAL:
    case APHELION_OP_NOT_EQUAL:
    case APHELION_OP_LESS:
    case APHELION_OP_LESS_EQUAL:
    case APHELION_OP_GREATER:
    case APHELION_OP_GREATER_EQUAL:
    case APHELION_OP_AND:
    case APHELION_OP_OR:
    case APHELION_OP_RETURN:
    case APHELION_OP_REVEAL:
    case APHELION_OP_TO_TEXT:
    case APHELION_OP_RECEIVE:
    case APHELION_OP_TO_WHOLE:
    case APHELION_OP_TO_DECIMAL:
    case APHELION_OP_LENGTH:
    case APHELION_OP_KIND:
    case APHELION_OP_INDEX:
    case APHELION_OP_NOP:
        if (instruction->operand != 0) {
            set_error(error, error_size, "unexpected operand for %s", aphelion_opcode_name(opcode));
            return false;
        }
        break;
    default:
        set_error(error, error_size, "unknown opcode %u", instruction->opcode);
        return false;
    }
    if (opcode != APHELION_OP_LIST && instruction->operand2 != 0) {
        set_error(error, error_size, "unexpected second operand for %s",
                  aphelion_opcode_name(opcode));
        return false;
    }
    return true;
}

bool aphelion_bytecode_validate(const AphelionBytecode *bytecode,
                                     char *error, size_t error_size) {
    size_t i;
    if (bytecode == NULL || bytecode->entry_function >= bytecode->function_count ||
        bytecode->functions == NULL ||
        (bytecode->constant_count != 0u && bytecode->constants == NULL) ||
        (bytecode->global_count != 0u && bytecode->globals == NULL)) {
        set_error(error, error_size, "invalid bytecode entry function");
        return false;
    }
    for (i = 0u; i < bytecode->constant_count; ++i) {
        AphelionValue value = bytecode->constants[i];
        if (value.type < APHELION_VALUE_HOLLOW || value.type > APHELION_VALUE_SILENT ||
            (value.type == APHELION_VALUE_TEXT && value.as.text == NULL) ||
            (value.type == APHELION_VALUE_DECIMAL && !isfinite(value.as.decimal)) ||
            (value.type == APHELION_VALUE_GLYPH &&
             (value.as.glyph > 0x10ffffu ||
              (value.as.glyph >= 0xd800u && value.as.glyph <= 0xdfffu)))) {
            set_error(error, error_size, "invalid constant value");
            return false;
        }
        if (bytecode->constants[i].type == APHELION_VALUE_FUNCTION &&
            bytecode->constants[i].as.function >= bytecode->function_count) {
            set_error(error, error_size, "function constant is out of range");
            return false;
        }
    }
    for (i = 0u; i < bytecode->function_count; ++i) {
        const AphelionFunction *function = &bytecode->functions[i];
        size_t j;
        if (function->code_count == 0u || function->code == NULL ||
            function->arity > function->local_count ||
            (i == bytecode->entry_function && function->arity != 0u)) {
            set_error(error, error_size, "function '%s' has empty code or invalid local metadata",
                      function->name != NULL ? function->name : "<function>");
            return false;
        }
        for (j = 0u; j < function->code_count; ++j) {
            if (!validate_instruction(bytecode, function, &function->code[j],
                                      error, error_size)) {
                return false;
            }
        }
    }
    return true;
}

bool aphelion_bytecode_read_file(AphelionBytecode *bytecode,
                                 const char *path,
                                 char *error,
                                 size_t error_size) {
    FILE *file;
    AphelionBytecode result;
    uint8_t magic[4];
    uint32_t version;
    uint32_t entry;
    uint32_t constants;
    uint32_t functions;
    uint32_t globals;
    uint32_t count;
    uint32_t j;
    size_t i;
    bool ok;
    char *text;
    long file_size;

    if (bytecode == NULL || path == NULL) {
        set_error(error, error_size, "invalid bytecode input arguments");
        return false;
    }
    file = aphelion_file_open(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "cannot open '%s' for reading", path);
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        file_size > 64L * 1024L * 1024L || fseek(file, 0L, SEEK_SET) != 0) {
        set_error(error, error_size, "bytecode file exceeds 64 MiB or is not seekable");
        (void)fclose(file);
        return false;
    }
    aphelion_bytecode_init(&result);
    if (!read_bytes(file, magic, sizeof(magic)) ||
        memcmp(magic, APHELION_BYTECODE_MAGIC, sizeof(magic)) != 0 ||
        !read_u32(file, &version) || version != APHELION_BYTECODE_VERSION ||
        !read_u32(file, &entry) || !read_u32(file, &constants) ||
        !read_u32(file, &functions) || !read_u32(file, &globals) ||
        constants > 1000000u || functions > 100000u || globals > 1000000u) {
        set_error(error, error_size, "invalid or unsupported bytecode header in '%s'", path);
        (void)fclose(file);
        aphelion_bytecode_destroy(&result);
        return false;
    }
    text = read_string(file, true, &ok, NULL);
    if (!ok) {
        set_error(error, error_size, "truncated bytecode source name in '%s'", path);
        (void)fclose(file);
        aphelion_bytecode_destroy(&result);
        return false;
    }
    result.source_name = text;
    result.entry_function = entry;
    for (i = 0u; i < constants; ++i) {
        AphelionValue value = aphelion_value_hollow();
        if (!read_value(file, &value) ||
            aphelion_bytecode_add_constant(&result, value) < 0) {
            aphelion_value_destroy(&value);
            set_error(error, error_size, "invalid constant table in '%s'", path);
            (void)fclose(file);
            aphelion_bytecode_destroy(&result);
            return false;
        }
        aphelion_value_destroy(&value);
    }
    for (i = 0u; i < globals; ++i) {
        text = read_string(file, false, &ok, NULL);
        if (!ok || text == NULL || text[0] == '\0' ||
            aphelion_bytecode_add_global(&result, text) != (int)i) {
            free(text);
            set_error(error, error_size, "invalid global table in '%s'", path);
            (void)fclose(file);
            aphelion_bytecode_destroy(&result);
            return false;
        }
        free(text);
    }
    for (i = 0u; i < functions; ++i) {
        uint32_t arity;
        uint32_t locals;
        int function_index;
        text = read_string(file, false, &ok, NULL);
        if (!ok || text == NULL || !read_u32(file, &arity) ||
            !read_u32(file, &locals) || !read_u32(file, &count) ||
            count > 10000000u || !file_has_bytes(file, (size_t)count * 17u)) {
            free(text);
            set_error(error, error_size, "invalid function table in '%s'", path);
            (void)fclose(file);
            aphelion_bytecode_destroy(&result);
            return false;
        }
        function_index = aphelion_bytecode_add_function(&result, text, arity, locals);
        free(text);
        if (function_index < 0) {
            set_error(error, error_size, "out of memory reading '%s'", path);
            (void)fclose(file);
            aphelion_bytecode_destroy(&result);
            return false;
        }
        for (j = 0u; j < count; ++j) {
            AphelionInstruction instruction;
            if (!read_u8(file, &instruction.opcode) ||
                !read_i32(file, &instruction.operand) ||
                !read_i32(file, &instruction.operand2) ||
                !read_u32(file, &instruction.line) ||
                !read_u32(file, &instruction.column) ||
                aphelion_function_emit(&result.functions[function_index],
                                       (AphelionOpcode)instruction.opcode,
                                       instruction.operand, instruction.operand2,
                                       instruction.line, instruction.column) < 0) {
                set_error(error, error_size, "invalid instruction stream in '%s'", path);
                (void)fclose(file);
                aphelion_bytecode_destroy(&result);
                return false;
            }
        }
    }
    if (!aphelion_bytecode_validate(&result, error, error_size)) {
        (void)fclose(file);
        aphelion_bytecode_destroy(&result);
        return false;
    }
    if (fgetc(file) != EOF || ferror(file)) {
        set_error(error, error_size, "trailing or unreadable data in '%s'", path);
        (void)fclose(file);
        aphelion_bytecode_destroy(&result);
        return false;
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "could not close '%s' after reading", path);
        aphelion_bytecode_destroy(&result);
        return false;
    }
    aphelion_bytecode_destroy(bytecode);
    *bytecode = result;
    return true;
}
