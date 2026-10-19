#ifndef APHELION_BYTECODE_H
#define APHELION_BYTECODE_H

/* Portable custom bytecode format shared by the compiler, VM and .apb files. */

#include "aphelion/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APHELION_BYTECODE_VERSION 1u
#define APHELION_BYTECODE_MAGIC "APB1"

typedef enum AphelionOpcode {
    APHELION_OP_HALT = 0,
    APHELION_OP_CONSTANT,
    APHELION_OP_HOLLOW,
    APHELION_OP_SILENT,
    APHELION_OP_POP,
    APHELION_OP_DUP,

    APHELION_OP_LOAD_LOCAL,
    APHELION_OP_STORE_LOCAL,
    APHELION_OP_LOAD_GLOBAL,
    APHELION_OP_STORE_GLOBAL,
    APHELION_OP_MAKE_FUNCTION,

    APHELION_OP_ADD,
    APHELION_OP_SUBTRACT,
    APHELION_OP_MULTIPLY,
    APHELION_OP_DIVIDE,
    APHELION_OP_REMAINDER,
    APHELION_OP_NEGATE,
    APHELION_OP_NOT,
    APHELION_OP_EQUAL,
    APHELION_OP_NOT_EQUAL,
    APHELION_OP_LESS,
    APHELION_OP_LESS_EQUAL,
    APHELION_OP_GREATER,
    APHELION_OP_GREATER_EQUAL,
    APHELION_OP_AND,
    APHELION_OP_OR,

    APHELION_OP_JUMP,
    APHELION_OP_JUMP_IF_FALSE,
    APHELION_OP_JUMP_IF_TRUE,

    APHELION_OP_CALL,
    APHELION_OP_RETURN,
    APHELION_OP_REVEAL,
    APHELION_OP_TO_TEXT,
    APHELION_OP_OUTPUT,
    APHELION_OP_BREAK,
    APHELION_OP_SKIP,
    APHELION_OP_NOP,
    APHELION_OP_RECEIVE,
    APHELION_OP_TO_WHOLE,
    APHELION_OP_TO_DECIMAL,
    APHELION_OP_LENGTH,
    APHELION_OP_KIND,
    APHELION_OP_ASSERT,
    APHELION_OP_LIST,  /* operand: count; operand2: primitive element type */
    APHELION_OP_INDEX,
    APHELION_OP_SOURCE /* operand: nonempty, NUL-free text constant; operand2: 0 */
} AphelionOpcode;

typedef struct AphelionInstruction {
    uint8_t opcode;
    int32_t operand;
    int32_t operand2;
    uint32_t line;
    uint32_t column;
} AphelionInstruction;

typedef struct AphelionFunction {
    char *name;
    uint32_t arity;
    uint32_t local_count;
    AphelionInstruction *code;
    size_t code_count;
    size_t code_capacity;
} AphelionFunction;

typedef struct AphelionBytecode {
    AphelionValue *constants;
    size_t constant_count;
    size_t constant_capacity;

    AphelionFunction *functions;
    size_t function_count;
    size_t function_capacity;
    uint32_t entry_function;

    char **globals;
    size_t global_count;
    size_t global_capacity;

    char *source_name;
} AphelionBytecode;

void aphelion_bytecode_init(AphelionBytecode *bytecode);
void aphelion_bytecode_destroy(AphelionBytecode *bytecode);
bool aphelion_bytecode_validate(const AphelionBytecode *bytecode,
                                char *error, size_t error_size);

int aphelion_bytecode_add_constant(AphelionBytecode *bytecode,
                                   AphelionValue value);
int aphelion_bytecode_add_global(AphelionBytecode *bytecode,
                                 const char *name);
int aphelion_bytecode_add_function(AphelionBytecode *bytecode,
                                   const char *name,
                                   uint32_t arity,
                                   uint32_t local_count);
bool aphelion_bytecode_set_source_name(AphelionBytecode *bytecode,
                                       const char *name);

int aphelion_function_emit(AphelionFunction *function,
                           AphelionOpcode opcode,
                           int32_t operand,
                           int32_t operand2,
                           uint32_t line,
                           uint32_t column);
bool aphelion_function_patch_operand(AphelionFunction *function,
                                     size_t instruction,
                                     int32_t operand);

const char *aphelion_opcode_name(AphelionOpcode opcode);
bool aphelion_bytecode_disassemble(const AphelionBytecode *bytecode,
                                   FILE *out);

/* .apb serialization.  The format is self-contained and little-endian. */
bool aphelion_bytecode_write_file(const AphelionBytecode *bytecode,
                                  const char *path,
                                  char *error,
                                  size_t error_size);
bool aphelion_bytecode_read_file(AphelionBytecode *bytecode,
                                 const char *path,
                                 char *error,
                                 size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_BYTECODE_H */
