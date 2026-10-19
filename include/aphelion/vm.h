#ifndef APHELION_VM_H
#define APHELION_VM_H

#include "aphelion/bytecode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APHELION_VM_DEFAULT_STACK_LIMIT 8192u
#define APHELION_VM_DEFAULT_CALL_LIMIT 256u
#define APHELION_VM_DEFAULT_INSTRUCTION_LIMIT UINT64_C(10000000)
#define APHELION_VM_ERROR_TEXT_SIZE 256u

typedef struct AphelionCallFrame {
    uint32_t function_index;
    size_t instruction;
    size_t base;
    const char *source_name; /* Borrowed from bytecode; never owned by the frame. */
} AphelionCallFrame;

typedef struct AphelionRuntimeError {
    char message[APHELION_VM_ERROR_TEXT_SIZE];
    const char *file; /* Borrowed from bytecode; valid until its destruction. */
    uint32_t line;
    uint32_t column;
} AphelionRuntimeError;

typedef struct AphelionVM {
    const AphelionBytecode *bytecode;
    AphelionValue *stack;
    size_t stack_count;
    size_t stack_capacity;
    size_t stack_limit;
    AphelionCallFrame *frames;
    size_t frame_count;
    size_t frame_capacity;
    size_t call_limit;
    uint64_t instruction_limit;
    AphelionValue *globals;
    size_t global_count;
    FILE *input;
    FILE *output;
    AphelionRuntimeError error;
    bool failed;
    bool halted;
} AphelionVM;

void aphelion_vm_init(AphelionVM *vm);
void aphelion_vm_destroy(AphelionVM *vm);
void aphelion_vm_set_limits(AphelionVM *vm,
                            size_t stack_limit,
                            size_t call_limit);
void aphelion_vm_set_input(AphelionVM *vm, FILE *input);
void aphelion_vm_set_instruction_limit(AphelionVM *vm, uint64_t limit);
void aphelion_vm_set_output(AphelionVM *vm, FILE *output);

/* Execute the entry function.  Returns false and fills error on failure. */
bool aphelion_vm_run(AphelionVM *vm, const AphelionBytecode *bytecode);
const AphelionRuntimeError *aphelion_vm_error(const AphelionVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_VM_H */
