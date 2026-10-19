#include "aphelion/compiler.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *compiler_strdup(const char *text) {
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

void aphelion_diagnostics_init(AphelionDiagnostics *diagnostics) {
    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
    }
}

void aphelion_diagnostics_destroy(AphelionDiagnostics *diagnostics) {
    size_t i;
    if (diagnostics == NULL) {
        return;
    }
    for (i = 0u; i < diagnostics->count; ++i) {
        free(diagnostics->items[i].message);
    }
    free(diagnostics->items);
    memset(diagnostics, 0, sizeof(*diagnostics));
}

bool aphelion_diagnostics_add(AphelionDiagnostics *diagnostics,
                              AphelionDiagnosticSeverity severity,
                              AphelionSourceLocation location,
                              const char *message) {
    AphelionDiagnostic *item;
    if (diagnostics == NULL || message == NULL) return false;
    if (!grow_array((void **)&diagnostics->items,
                    &diagnostics->capacity,
                    diagnostics->count + 1u,
                    sizeof(*diagnostics->items))) {
        diagnostics->allocation_failed = true;
        return false;
    }
    item = &diagnostics->items[diagnostics->count];
    item->message = compiler_strdup(message);
    if (item->message == NULL) {
        diagnostics->allocation_failed = true;
        return false;
    }
    item->severity = severity;
    item->location = location;
    diagnostics->count += 1u;
    return true;
}

void aphelion_diagnostics_print(const AphelionDiagnostics *diagnostics,
                                FILE *out) {
    size_t i;
    if (diagnostics == NULL || out == NULL) {
        return;
    }
    if (diagnostics->allocation_failed)
        (void)fputs("<source>:1:1: out of memory recording compiler diagnostics\n", out);
    for (i = 0u; i < diagnostics->count; ++i) {
        const AphelionDiagnostic *item = &diagnostics->items[i];
        (void)fprintf(out, "%s:%" PRIu32 ":%" PRIu32 ": %s%s\n",
                      item->location.file != NULL ? item->location.file : "<source>",
                      item->location.line == 0u ? 1u : item->location.line,
                      item->location.column == 0u ? 1u : item->location.column,
                      item->severity == APHELION_DIAGNOSTIC_WARNING ? "warning: " : "",
                      item->message != NULL ? item->message : "<diagnostic>");
    }
}

bool aphelion_diagnostics_has_errors(const AphelionDiagnostics *diagnostics) {
    size_t i;
    if (diagnostics == NULL) {
        return false;
    }
    if (diagnostics->allocation_failed) return true;
    for (i = 0u; i < diagnostics->count; ++i) {
        if (diagnostics->items[i].severity == APHELION_DIAGNOSTIC_ERROR) {
            return true;
        }
    }
    return false;
}

void aphelion_compiler_options_default(AphelionCompilerOptions *options) {
    if (options == NULL) {
        return;
    }
    options->max_functions = 100000u;
    options->max_constants = 1000000u;
    options->max_globals = 1000000u;
    options->echo_expressions = false;
}

typedef struct CompileSymbol {
    char *name;
    uint32_t slot;
    const void *ast_symbol;
} CompileSymbol;

typedef struct LoopTarget {
    size_t continue_target;
    size_t *break_jumps;
    size_t break_count;
    size_t break_capacity;
    struct LoopTarget *parent;
} LoopTarget;

typedef struct CompileFunction {
    const AstFunction *ast;
    int bytecode_index;
    CompileSymbol *locals;
    size_t local_count;
    size_t local_capacity;
    LoopTarget *loop;
    bool entry;
} CompileFunction;

typedef struct CompilerState {
    const AstProgram *program;
    const AphelionCompilerOptions *options;
    AphelionBytecode *bytecode;
    AphelionDiagnostics *diagnostics;
    CompileFunction *functions;
    size_t function_count;
    CompileFunction *current;
    bool entry_top_level;
} CompilerState;

static AphelionSourceLocation source_location(const AstProgram *program,
                                              SourceLocation location) {
    AphelionSourceLocation result;
    result.file = location.filename != NULL
                    ? location.filename
                    : (program != NULL ? program->filename : "<source>");
    result.line = (uint32_t)(location.line > UINT32_MAX ? UINT32_MAX : location.line);
    result.column = (uint32_t)(location.column > UINT32_MAX ? UINT32_MAX : location.column);
    return result;
}

static AphelionSourceLocation expr_location(CompilerState *state,
                                            const AstExpr *expression) {
    return source_location(state->program, expression != NULL
                                             ? expression->span.start : (SourceLocation){0});
}

static AphelionSourceLocation stmt_location(CompilerState *state,
                                            const AstStmt *statement) {
    return source_location(state->program, statement != NULL
                                             ? statement->span.start : (SourceLocation){0});
}

static AphelionSourceLocation function_location(CompilerState *state,
                                                const AstFunction *function) {
    return source_location(state->program, function != NULL
                                             ? function->span.start : (SourceLocation){0});
}

static void compile_error_at(CompilerState *state,
                             AphelionSourceLocation location,
                             const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    (void)aphelion_diagnostics_add(state->diagnostics,
                                   APHELION_DIAGNOSTIC_ERROR,
                                   location,
                                   message);
}

static void compile_error_expr(CompilerState *state, const AstExpr *expr,
                               const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    (void)aphelion_diagnostics_add(state->diagnostics,
                                   APHELION_DIAGNOSTIC_ERROR,
                                   expr_location(state, expr),
                                   message);
}

static int emit_instruction(CompilerState *state, AphelionOpcode opcode,
                            int32_t operand, int32_t operand2,
                            AphelionSourceLocation location) {
    AphelionFunction *function;
    if (state == NULL || state->current == NULL ||
        state->current->bytecode_index < 0 ||
        (size_t)state->current->bytecode_index >= state->bytecode->function_count) {
        return -1;
    }
    function = &state->bytecode->functions[state->current->bytecode_index];
    return aphelion_function_emit(function, opcode, operand, operand2,
                                  location.line, location.column);
}

static bool emit_checked(CompilerState *state, AphelionOpcode opcode,
                         int32_t operand, int32_t operand2,
                         AphelionSourceLocation location) {
    if (emit_instruction(state, opcode, operand, operand2, location) < 0) {
        compile_error_at(state, location, "out of memory emitting bytecode");
        return false;
    }
    return true;
}

static int add_constant(CompilerState *state, AphelionValue value,
                        AphelionSourceLocation location) {
    int index;
    if (state->options != NULL &&
        state->bytecode->constant_count >= state->options->max_constants) {
        compile_error_at(state, location, "constant pool limit exceeded");
        return -1;
    }
    index = aphelion_bytecode_add_constant(state->bytecode, value);
    if (index < 0) {
        compile_error_at(state, location, "out of memory adding constant");
    }
    return index;
}

static int add_global(CompilerState *state, const char *name,
                      AphelionSourceLocation location) {
    int index;
    if (state->options != NULL &&
        state->bytecode->global_count >= state->options->max_globals) {
        compile_error_at(state, location, "global limit exceeded");
        return -1;
    }
    index = aphelion_bytecode_add_global(state->bytecode, name);
    if (index < 0) {
        compile_error_at(state, location, "out of memory adding global '%s'", name);
    }
    return index;
}

static bool emit_source(CompilerState *state, AphelionSourceLocation location) {
    AphelionValue value;
    size_t i, length;
    int constant;
    if (state->program->source_file_count == 0u) return true;
    if (location.file == NULL || location.file[0] == '\0') {
        compile_error_at(state, location, "linked source location has no filename");
        return false;
    }
    length = strlen(location.file);
    /* Reuse filenames across function entries and initialization boundaries. */
    for (i = 0u; i < state->bytecode->constant_count; ++i) {
        value = state->bytecode->constants[i];
        if (value.type == APHELION_VALUE_TEXT && value.as.text != NULL &&
            value.as.text->length == length &&
            memcmp(value.as.text->bytes, location.file, length) == 0) {
            return emit_checked(state, APHELION_OP_SOURCE, (int32_t)i, 0, location);
        }
    }
    value = aphelion_value_text_n(location.file, length);
    if (value.type != APHELION_VALUE_TEXT) {
        compile_error_at(state, location, "out of memory creating source filename");
        return false;
    }
    constant = add_constant(state, value, location);
    aphelion_value_destroy(&value);
    return constant >= 0 &&
           emit_checked(state, APHELION_OP_SOURCE, constant, 0, location);
}

static CompileFunction *find_function_context(CompilerState *state,
                                              const char *name) {
    size_t i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0u; i < state->function_count; ++i) {
        if (state->functions[i].ast != NULL &&
            state->functions[i].ast->name != NULL &&
            strcmp(state->functions[i].ast->name, name) == 0) {
            return &state->functions[i];
        }
    }
    return NULL;
}

static int find_local(const CompileFunction *function, const char *name) {
    size_t i;
    if (function == NULL || name == NULL) {
        return -1;
    }
    for (i = function->local_count; i > 0u; --i) {
        if (strcmp(function->locals[i - 1u].name, name) == 0) {
            return (int)function->locals[i - 1u].slot;
        }
    }
    return -1;
}

static int find_local_symbol(const CompileFunction *function,
                             const void *ast_symbol) {
    size_t i;
    if (function == NULL || ast_symbol == NULL) return -1;
    for (i = function->local_count; i > 0u; --i) {
        if (function->locals[i - 1u].ast_symbol == ast_symbol) {
            return (int)function->locals[i - 1u].slot;
        }
    }
    return -1;
}

static int add_local(CompilerState *state, CompileFunction *function,
                     const char *name, const void *ast_symbol,
                     AphelionSourceLocation location) {
    CompileSymbol *symbol;
    size_t i;
    if (function == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < function->local_count; ++i) {
        if ((ast_symbol != NULL && function->locals[i].ast_symbol == ast_symbol) ||
            (ast_symbol == NULL && strcmp(function->locals[i].name, name) == 0)) {
            return (int)function->locals[i].slot;
        }
    }
    if (!grow_array((void **)&function->locals,
                    &function->local_capacity,
                    function->local_count + 1u,
                    sizeof(*function->locals))) {
        compile_error_at(state, location, "out of memory adding local '%s'", name);
        return -1;
    }
    symbol = &function->locals[function->local_count];
    symbol->name = compiler_strdup(name);
    if (symbol->name == NULL) {
        compile_error_at(state, location, "out of memory adding local '%s'", name);
        return -1;
    }
    symbol->slot = (uint32_t)function->local_count;
    symbol->ast_symbol = ast_symbol;
    function->local_count += 1u;
    return (int)symbol->slot;
}

static int find_global(const CompilerState *state, const char *name) {
    size_t i;
    for (i = 0u; i < state->bytecode->global_count; ++i) {
        if (strcmp(state->bytecode->globals[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void free_function_contexts(CompileFunction *functions,
                                   size_t function_count) {
    size_t i;
    if (functions == NULL) {
        return;
    }
    for (i = 0u; i < function_count; ++i) {
        size_t j;
        for (j = 0u; j < functions[i].local_count; ++j) {
            free(functions[i].locals[j].name);
        }
        free(functions[i].locals);
    }
    free(functions);
}

static bool collect_locals_in_block(CompilerState *state,
                                    CompileFunction *function,
                                    const AstBlock *block);

static bool collect_traverse_slots(CompilerState *state,
                                   CompileFunction *function,
                                   const AstStmt *statement) {
    const AstStmt *binding = statement->as.traverse.binding;
    AphelionSourceLocation location = stmt_location(state, statement);
    /* Hidden names cannot be source identifiers; all three slots are resolved
     * by declaration identity, including in the entry frame and nested loops. */
    return binding != NULL &&
           add_local(state, function, binding->as.var_decl.name, binding,
                     location) >= 0 &&
           add_local(state, function, "<traverse snapshot>", statement,
                     location) >= 0 &&
           add_local(state, function, "<traverse index>",
                     &statement->as.traverse.iterable, location) >= 0;
}

static bool collect_locals_in_stmt(CompilerState *state,
                                   CompileFunction *function,
                                   const AstStmt *statement) {
    if (statement == NULL) {
        return true;
    }
    switch (statement->kind) {
    case AST_STMT_SUMMON:
        compile_error_at(state, stmt_location(state, statement),
                         "unresolved summon must be linked before compilation");
        return false;
    case AST_STMT_VAR_DECL:
        if (function->entry) {
            return true;
        }
        return add_local(state, function, statement->as.var_decl.name, statement,
                         source_location(state->program, statement->span.start)) >= 0;
    case AST_STMT_BLOCK:
        return collect_locals_in_block(state, function, statement->as.block.block);
    case AST_STMT_WHEN:
        return collect_locals_in_block(state, function, statement->as.when_stmt.then_branch) &&
               collect_locals_in_block(state, function, statement->as.when_stmt.else_branch);
    case AST_STMT_CYCLE:
        return collect_locals_in_block(state, function, statement->as.cycle.body);
    case AST_STMT_TRAVERSE:
        return collect_traverse_slots(state, function, statement) &&
               collect_locals_in_block(state, function, statement->as.traverse.body);
    case AST_STMT_EXPRESSION:
    case AST_STMT_YIELD:
    case AST_STMT_ESCAPE:
    case AST_STMT_SKIP:
        return true;
    default:
        return true;
    }
}

static bool collect_locals_in_block(CompilerState *state,
                                    CompileFunction *function,
                                    const AstBlock *block) {
    size_t i;
    if (block == NULL) {
        return true;
    }
    for (i = 0u; i < block->statement_count; ++i) {
        if (!collect_locals_in_stmt(state, function, block->statements[i])) {
            return false;
        }
    }
    return true;
}

/* Top-level declarations are globals, but declarations nested inside a
 * top-level block/branch still belong to the entry frame's lexical scope. */
static bool collect_entry_nested_locals_in_stmt(CompilerState *state,
                                                CompileFunction *function,
                                                const AstStmt *statement);

static bool collect_entry_nested_locals_in_block(CompilerState *state,
                                                 CompileFunction *function,
                                                 const AstBlock *block) {
    size_t i;
    if (block == NULL) return true;
    for (i = 0u; i < block->statement_count; ++i) {
        if (!collect_entry_nested_locals_in_stmt(state, function,
                                                 block->statements[i])) {
            return false;
        }
    }
    return true;
}

static bool collect_entry_nested_locals_in_stmt(CompilerState *state,
                                                CompileFunction *function,
                                                const AstStmt *statement) {
    if (statement == NULL) return true;
    switch (statement->kind) {
    case AST_STMT_VAR_DECL:
        return add_local(state, function, statement->as.var_decl.name, statement,
                         source_location(state->program, statement->span.start)) >= 0;
    case AST_STMT_BLOCK:
        return collect_entry_nested_locals_in_block(state, function,
                                                    statement->as.block.block);
    case AST_STMT_WHEN:
        return collect_entry_nested_locals_in_block(state, function,
                                                    statement->as.when_stmt.then_branch) &&
               collect_entry_nested_locals_in_block(state, function,
                                                    statement->as.when_stmt.else_branch);
    case AST_STMT_CYCLE:
        return collect_entry_nested_locals_in_block(state, function,
                                                    statement->as.cycle.body);
    case AST_STMT_TRAVERSE:
        return collect_traverse_slots(state, function, statement) &&
               collect_entry_nested_locals_in_block(state, function,
                                                    statement->as.traverse.body);
    case AST_STMT_EXPRESSION:
    case AST_STMT_YIELD:
    case AST_STMT_ESCAPE:
    case AST_STMT_SKIP:
        return true;
    default:
        return true;
    }
}

static int resolve_name(CompilerState *state, const char *name,
                        const void *ast_symbol,
                        bool *is_local, bool *is_global) {
    int slot;
    if (is_local != NULL) *is_local = false;
    if (is_global != NULL) *is_global = false;
    slot = ast_symbol != NULL
             ? find_local_symbol(state->current, ast_symbol)
             : find_local(state->current, name);
    if (slot >= 0) {
        if (is_local != NULL) *is_local = true;
        return slot;
    }
    slot = find_global(state, name);
    if (slot >= 0) {
        if (is_global != NULL) *is_global = true;
        return slot;
    }
    return -1;
}

static bool compile_expr(CompilerState *state, const AstExpr *expression);
static bool compile_stmt(CompilerState *state, const AstStmt *statement);

static bool compile_as(CompilerState *state, const AstExpr *expression,
                       TypeKind expected) {
    if (!compile_expr(state, expression)) return false;
    if (expression != NULL && expected == TYPE_DECIMAL &&
        expression->inferred_type == TYPE_WHOLE) {
        return emit_checked(state, APHELION_OP_TO_DECIMAL, 0, 0,
                            expr_location(state, expression));
    }
    return true;
}

static bool compile_literal(CompilerState *state, const AstExpr *expression) {
    AphelionValue value;
    int constant;
    switch (expression->as.literal.kind) {
    case AST_LITERAL_WHOLE:
        value = aphelion_value_whole(expression->as.literal.whole);
        break;
    case AST_LITERAL_DECIMAL:
        value = aphelion_value_decimal(expression->as.literal.decimal);
        break;
    case AST_LITERAL_SIGNAL:
        value = aphelion_value_signal(expression->as.literal.signal);
        break;
    case AST_LITERAL_TEXT:
        value = aphelion_value_text_n(expression->as.literal.text,
                                      expression->as.literal.text_length);
        if (value.type != APHELION_VALUE_TEXT) {
            compile_error_expr(state, expression, "out of memory creating text literal");
            return false;
        }
        break;
    case AST_LITERAL_GLYPH:
        value = aphelion_value_glyph(expression->as.literal.glyph);
        break;
    case AST_LITERAL_HOLLOW:
        value = aphelion_value_hollow();
        break;
    default:
        compile_error_expr(state, expression, "unsupported literal");
        return false;
    }
    constant = add_constant(state, value, expr_location(state, expression));
    aphelion_value_destroy(&value);
    return constant >= 0 && emit_checked(state, APHELION_OP_CONSTANT, constant, 0,
                                         expr_location(state, expression));
}

static AphelionOpcode binary_opcode(AstBinaryOp op) {
    switch (op) {
    case AST_BINARY_ADD: return APHELION_OP_ADD;
    case AST_BINARY_SUBTRACT: return APHELION_OP_SUBTRACT;
    case AST_BINARY_MULTIPLY: return APHELION_OP_MULTIPLY;
    case AST_BINARY_DIVIDE: return APHELION_OP_DIVIDE;
    case AST_BINARY_REMAINDER: return APHELION_OP_REMAINDER;
    case AST_BINARY_EQUAL: return APHELION_OP_EQUAL;
    case AST_BINARY_NOT_EQUAL: return APHELION_OP_NOT_EQUAL;
    case AST_BINARY_LESS: return APHELION_OP_LESS;
    case AST_BINARY_LESS_EQUAL: return APHELION_OP_LESS_EQUAL;
    case AST_BINARY_GREATER: return APHELION_OP_GREATER;
    case AST_BINARY_GREATER_EQUAL: return APHELION_OP_GREATER_EQUAL;
    case AST_BINARY_AND: return APHELION_OP_AND;
    case AST_BINARY_OR: return APHELION_OP_OR;
    default: return APHELION_OP_NOP;
    }
}

static bool compile_name(CompilerState *state, const AstExpr *expression) {
    bool local;
    bool global;
    int slot = resolve_name(state, expression->as.name.name,
                            expression->resolved_symbol, &local, &global);
    CompileFunction *function;
    if (slot >= 0) {
        return emit_checked(state, local ? APHELION_OP_LOAD_LOCAL
                                         : APHELION_OP_LOAD_GLOBAL,
                            slot, 0, expr_location(state, expression));
    }
    function = find_function_context(state, expression->as.name.name);
    if (function != NULL) {
        return emit_checked(state, APHELION_OP_MAKE_FUNCTION,
                            function->bytecode_index, 0,
                            expr_location(state, expression));
    }
    compile_error_expr(state, expression, "unknown name '%s'",
                       expression->as.name.name != NULL
                         ? expression->as.name.name : "<null>");
    return false;
}

static bool compile_assignment(CompilerState *state, const AstExpr *expression) {
    const AstExpr *target = expression->as.assignment.target;
    bool local;
    bool global;
    int slot;
    AphelionOpcode operation;
    while (target != NULL && target->kind == AST_EXPR_GROUP) {
        target = target->as.group.inner;
    }
    if (target == NULL || target->kind != AST_EXPR_NAME) {
        compile_error_expr(state, expression, "assignment target must be a name");
        return false;
    }
    slot = resolve_name(state, target->as.name.name, target->resolved_symbol,
                        &local, &global);
    if (slot < 0) {
        compile_error_expr(state, expression, "unknown assignment target '%s'",
                           target->as.name.name != NULL
                             ? target->as.name.name : "<null>");
        return false;
    }
    if (expression->as.assignment.op == AST_ASSIGN_SET) {
        if (!compile_as(state, expression->as.assignment.value,
                         target->inferred_type)) return false;
    } else {
        if (!compile_name(state, target) ||
            !compile_expr(state, expression->as.assignment.value)) return false;
        switch (expression->as.assignment.op) {
        case AST_ASSIGN_ADD: operation = APHELION_OP_ADD; break;
        case AST_ASSIGN_SUBTRACT: operation = APHELION_OP_SUBTRACT; break;
        case AST_ASSIGN_MULTIPLY: operation = APHELION_OP_MULTIPLY; break;
        case AST_ASSIGN_DIVIDE: operation = APHELION_OP_DIVIDE; break;
        case AST_ASSIGN_REMAINDER: operation = APHELION_OP_REMAINDER; break;
        default: operation = APHELION_OP_NOP; break;
        }
        if (!emit_checked(state, operation, 0, 0, expr_location(state, expression))) {
            return false;
        }
    }
    if (!emit_checked(state, APHELION_OP_DUP, 0, 0, expr_location(state, expression))) {
        return false;
    }
    return emit_checked(state, local ? APHELION_OP_STORE_LOCAL
                                     : APHELION_OP_STORE_GLOBAL,
                        slot, 0, expr_location(state, expression));
}

static bool compile_call(CompilerState *state, const AstExpr *expression) {
    const AstExpr *callee = expression->as.call.callee;
    size_t i;
    while (callee != NULL && callee->kind == AST_EXPR_GROUP) callee = callee->as.group.inner;
    if (callee != NULL && callee->kind == AST_EXPR_NAME &&
        callee->as.name.name != NULL &&
        strcmp(callee->as.name.name, "reveal") == 0) {
        if (expression->as.call.argument_count != 1u) {
            compile_error_expr(state, expression, "reveal expects one argument");
            return false;
        }
        if (!compile_expr(state, expression->as.call.arguments[0])) return false;
        return emit_checked(state, APHELION_OP_REVEAL, 0, 0,
                            expr_location(state, expression));
    }
    if (callee != NULL && callee->kind == AST_EXPR_NAME &&
        callee->as.name.name != NULL &&
        strcmp(callee->as.name.name, "to_text") == 0) {
        if (expression->as.call.argument_count != 1u) {
            compile_error_expr(state, expression, "to_text expects one argument");
            return false;
        }
        if (!compile_expr(state, expression->as.call.arguments[0])) return false;
        return emit_checked(state, APHELION_OP_TO_TEXT, 0, 0,
                            expr_location(state, expression));
    }
    if (callee != NULL && callee->kind == AST_EXPR_NAME &&
        callee->as.name.name != NULL &&
        strcmp(callee->as.name.name, "receive") == 0) {
        if (expression->as.call.argument_count != 0u) {
            compile_error_expr(state, expression, "receive expects no arguments");
            return false;
        }
        return emit_checked(state, APHELION_OP_RECEIVE, 0, 0,
                            expr_location(state, expression));
    }
    if (callee != NULL && callee->kind == AST_EXPR_NAME &&
        callee->as.name.name != NULL &&
        (strcmp(callee->as.name.name, "to_whole") == 0 ||
         strcmp(callee->as.name.name, "to_decimal") == 0)) {
        if (expression->as.call.argument_count != 1u) {
            compile_error_expr(state, expression, "%s expects one argument",
                               callee->as.name.name);
            return false;
        }
        if (!compile_expr(state, expression->as.call.arguments[0])) return false;
        return emit_checked(state,
                            strcmp(callee->as.name.name, "to_whole") == 0
                              ? APHELION_OP_TO_WHOLE : APHELION_OP_TO_DECIMAL,
                            0, 0, expr_location(state, expression));
    }
    if (callee != NULL && callee->kind == AST_EXPR_NAME &&
        callee->as.name.name != NULL &&
        (strcmp(callee->as.name.name, "length") == 0 ||
         strcmp(callee->as.name.name, "kind") == 0 ||
         strcmp(callee->as.name.name, "assert") == 0)) {
        const char *builtin = callee->as.name.name;
        bool assertion = strcmp(builtin, "assert") == 0;
        size_t count = expression->as.call.argument_count;
        AphelionOpcode opcode = assertion ? APHELION_OP_ASSERT
                                  : strcmp(builtin, "length") == 0
                                      ? APHELION_OP_LENGTH : APHELION_OP_KIND;
        if (assertion ? (count != 1u && count != 2u) : count != 1u) {
            compile_error_expr(state, expression, "%s expects %s", builtin,
                               assertion ? "one or two arguments" : "one argument");
            return false;
        }
        /* Every argument is evaluated in source order, including the message
         * of a successful assertion.  The opcode consumes the resulting values. */
        for (i = 0u; i < count; ++i) {
            if (!compile_expr(state, expression->as.call.arguments[i])) return false;
        }
        return emit_checked(state, opcode, assertion ? (int32_t)count : 0, 0,
                            expr_location(state, expression));
    }
    if (callee == NULL || !compile_expr(state, callee)) return false;
    for (i = 0u; i < expression->as.call.argument_count; ++i) {
        const AstFunction *function = (const AstFunction *)expression->resolved_symbol;
        TypeKind expected = function != NULL && i < function->parameter_count
                             ? function->parameters[i].type.kind : TYPE_UNKNOWN;
        if (!compile_as(state, expression->as.call.arguments[i], expected)) return false;
    }
    if (expression->as.call.argument_count > INT32_MAX) {
        compile_error_expr(state, expression, "too many call arguments");
        return false;
    }
    return emit_checked(state, APHELION_OP_CALL,
                        (int32_t)expression->as.call.argument_count, 0,
                        expr_location(state, expression));
}

static bool compile_list(CompilerState *state, const AstExpr *expression) {
    size_t i;
    AphelionValueType tag;
    if (expression->as.list.element_count > INT32_MAX) {
        compile_error_expr(state, expression, "too many list elements");
        return false;
    }
    if (expression->as.list.element_count == 0u) {
        tag = APHELION_VALUE_HOLLOW;
    } else {
        switch (type_kind_list_element(expression->inferred_type)) {
        case TYPE_WHOLE: tag = APHELION_VALUE_WHOLE; break;
        case TYPE_DECIMAL: tag = APHELION_VALUE_DECIMAL; break;
        case TYPE_SIGNAL: tag = APHELION_VALUE_SIGNAL; break;
        case TYPE_TEXT: tag = APHELION_VALUE_TEXT; break;
        case TYPE_GLYPH: tag = APHELION_VALUE_GLYPH; break;
        default:
            compile_error_expr(state, expression, "list requires a scalar element type");
            return false;
        }
    }
    for (i = 0u; i < expression->as.list.element_count; ++i) {
        if (!compile_expr(state, expression->as.list.elements[i])) return false;
    }
    return emit_checked(state, APHELION_OP_LIST,
                        (int32_t)expression->as.list.element_count, (int32_t)tag,
                        expr_location(state, expression));
}

static bool compile_expr(CompilerState *state, const AstExpr *expression) {
    size_t jump;
    int emitted_jump;
    AphelionFunction *function;
    if (expression == NULL) {
        return emit_checked(state, APHELION_OP_HOLLOW, 0, 0,
                            source_location(state->program, (SourceLocation){0}));
    }
    switch (expression->kind) {
    case AST_EXPR_LIST:
        return compile_list(state, expression);
    case AST_EXPR_INDEX:
        return compile_expr(state, expression->as.index.target) &&
               compile_expr(state, expression->as.index.index) &&
               emit_checked(state, APHELION_OP_INDEX, 0, 0,
                            expr_location(state, expression));
    case AST_EXPR_LITERAL:
        return compile_literal(state, expression);
    case AST_EXPR_NAME:
        return compile_name(state, expression);
    case AST_EXPR_GROUP:
        return compile_expr(state, expression->as.group.inner);
    case AST_EXPR_ASSIGN:
        return compile_assignment(state, expression);
    case AST_EXPR_UNARY:
        if (!compile_expr(state, expression->as.unary.operand)) return false;
        if (expression->as.unary.op == AST_UNARY_PLUS) {
            return true;
        }
        return emit_checked(state,
                            expression->as.unary.op == AST_UNARY_NOT
                              ? APHELION_OP_NOT : APHELION_OP_NEGATE,
                            0, 0, expr_location(state, expression));
    case AST_EXPR_BINARY:
        if (expression->as.binary.op == AST_BINARY_AND ||
            expression->as.binary.op == AST_BINARY_OR) {
            if (!compile_expr(state, expression->as.binary.left)) return false;
            emitted_jump = emit_instruction(state,
                         expression->as.binary.op == AST_BINARY_AND
                           ? APHELION_OP_JUMP_IF_FALSE : APHELION_OP_JUMP_IF_TRUE,
                         0, 0, expr_location(state, expression));
            if (emitted_jump < 0) {
                compile_error_expr(state, expression, "out of memory emitting jump");
                return false;
            }
            jump = (size_t)emitted_jump;
            if (!emit_checked(state, APHELION_OP_POP, 0, 0,
                              expr_location(state, expression)) ||
                !compile_expr(state, expression->as.binary.right)) return false;
            function = &state->bytecode->functions[state->current->bytecode_index];
            (void)aphelion_function_patch_operand(function, jump,
                                                  (int32_t)function->code_count);
            return true;
        }
        if (!compile_expr(state, expression->as.binary.left) ||
            !compile_expr(state, expression->as.binary.right)) return false;
        return emit_checked(state, binary_opcode(expression->as.binary.op),
                            0, 0, expr_location(state, expression));
    case AST_EXPR_CALL:
        return compile_call(state, expression);
    default:
        compile_error_expr(state, expression, "unsupported expression kind");
        return false;
    }
}

static bool compile_block(CompilerState *state, const AstBlock *block) {
    size_t i;
    bool saved_entry_top_level;
    if (block == NULL) {
        return true;
    }
    saved_entry_top_level = state->entry_top_level;
    if (state->current->entry) state->entry_top_level = false;
    for (i = 0u; i < block->statement_count; ++i) {
        if (!compile_stmt(state, block->statements[i])) {
            state->entry_top_level = saved_entry_top_level;
            return false;
        }
    }
    state->entry_top_level = saved_entry_top_level;
    return true;
}

static bool add_break_jump(CompilerState *state, LoopTarget *loop,
                           size_t jump) {
    if (!grow_array((void **)&loop->break_jumps, &loop->break_capacity,
                    loop->break_count + 1u, sizeof(*loop->break_jumps))) {
        compile_error_at(state, source_location(state->program, (SourceLocation){0}),
                         "out of memory recording loop exit");
        return false;
    }
    loop->break_jumps[loop->break_count++] = jump;
    return true;
}

static bool compile_traverse(CompilerState *state, const AstStmt *statement) {
    AphelionSourceLocation location = stmt_location(state, statement);
    AphelionFunction *function;
    LoopTarget loop;
    size_t initial_jump, condition_jump, i;
    int jump, zero, one;
    int binding = find_local_symbol(state->current, statement->as.traverse.binding);
    int snapshot = find_local_symbol(state->current, statement);
    int index = find_local_symbol(state->current, &statement->as.traverse.iterable);
    int slots[3];
    if (binding < 0 || snapshot < 0 || index < 0) {
        compile_error_at(state, location, "missing traversal local slots");
        return false;
    }
    zero = add_constant(state, aphelion_value_whole(0), location);
    one = add_constant(state, aphelion_value_whole(1), location);
    if (zero < 0 || one < 0 ||
        !compile_expr(state, statement->as.traverse.iterable) ||
        !emit_checked(state, APHELION_OP_STORE_LOCAL, snapshot, 0, location) ||
        !emit_checked(state, APHELION_OP_CONSTANT, zero, 0, location) ||
        !emit_checked(state, APHELION_OP_STORE_LOCAL, index, 0, location)) return false;
    jump = emit_instruction(state, APHELION_OP_JUMP, 0, 0, location);
    if (jump < 0) {
        compile_error_at(state, location, "out of memory emitting traversal jump");
        return false;
    }
    initial_jump = (size_t)jump;
    memset(&loop, 0, sizeof(loop));
    loop.parent = state->current->loop;
    /* Put the increment before the test, with the first entry jumping over
     * it. Thus skip has a known target and always advances the current item. */
    loop.continue_target =
        state->bytecode->functions[state->current->bytecode_index].code_count;
    if (!emit_checked(state, APHELION_OP_LOAD_LOCAL, index, 0, location) ||
        !emit_checked(state, APHELION_OP_CONSTANT, one, 0, location) ||
        !emit_checked(state, APHELION_OP_ADD, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_STORE_LOCAL, index, 0, location)) return false;
    function = &state->bytecode->functions[state->current->bytecode_index];
    (void)aphelion_function_patch_operand(function, initial_jump,
                                         (int32_t)function->code_count);
    if (!emit_checked(state, APHELION_OP_LOAD_LOCAL, index, 0, location) ||
        !emit_checked(state, APHELION_OP_LOAD_LOCAL, snapshot, 0, location) ||
        !emit_checked(state, APHELION_OP_LENGTH, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_LESS, 0, 0, location)) return false;
    jump = emit_instruction(state, APHELION_OP_JUMP_IF_FALSE, 0, 0, location);
    if (jump < 0) {
        compile_error_at(state, location, "out of memory emitting traversal jump");
        return false;
    }
    condition_jump = (size_t)jump;
    if (!emit_checked(state, APHELION_OP_POP, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_LOAD_LOCAL, snapshot, 0, location) ||
        !emit_checked(state, APHELION_OP_LOAD_LOCAL, index, 0, location) ||
        !emit_checked(state, APHELION_OP_INDEX, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_STORE_LOCAL, binding, 0, location)) return false;
    state->current->loop = &loop;
    if (!compile_block(state, statement->as.traverse.body) ||
        !emit_checked(state, APHELION_OP_JUMP, (int32_t)loop.continue_target,
                      0, location)) goto fail;
    function = &state->bytecode->functions[state->current->bytecode_index];
    (void)aphelion_function_patch_operand(function, condition_jump,
                                         (int32_t)function->code_count);
    /* Conditional jumps peek. Escape has already consumed the true condition,
     * so it skips this POP but must still release the loop-private values. */
    if (!emit_checked(state, APHELION_OP_POP, 0, 0, location)) goto fail;
    function = &state->bytecode->functions[state->current->bytecode_index];
    for (i = 0u; i < loop.break_count; ++i) {
        (void)aphelion_function_patch_operand(function, loop.break_jumps[i],
                                             (int32_t)function->code_count);
    }
    slots[0] = binding;
    slots[1] = snapshot;
    slots[2] = index;
    for (i = 0u; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        if (!emit_checked(state, APHELION_OP_HOLLOW, 0, 0, location) ||
            !emit_checked(state, APHELION_OP_STORE_LOCAL, slots[i], 0, location)) goto fail;
    }
    state->current->loop = loop.parent;
    free(loop.break_jumps);
    return true;
fail:
    state->current->loop = loop.parent;
    free(loop.break_jumps);
    return false;
}

static bool compile_stmt(CompilerState *state, const AstStmt *statement) {
    AphelionSourceLocation location;
    size_t jump_false;
    size_t jump_end;
    size_t loop_start;
    size_t i;
    int emitted_jump;
    AphelionFunction *function;
    LoopTarget loop;
    int slot;
    if (statement == NULL) return true;
    location = stmt_location(state, statement);
    switch (statement->kind) {
    case AST_STMT_SUMMON:
        compile_error_at(state, location,
                         "unresolved summon must be linked before compilation");
        return false;
    case AST_STMT_VAR_DECL:
        if (state->current->entry && state->entry_top_level) {
            slot = find_global(state, statement->as.var_decl.name);
            if (slot < 0) slot = add_global(state, statement->as.var_decl.name, location);
        } else {
            slot = find_local_symbol(state->current, statement);
            if (slot < 0) slot = find_local(state->current, statement->as.var_decl.name);
        }
        if (slot < 0) return false;
        if (!compile_as(state, statement->as.var_decl.initializer,
                         statement->as.var_decl.type.kind) ||
            !emit_checked(state, state->current->entry && state->entry_top_level
                                      ? APHELION_OP_STORE_GLOBAL
                                                         : APHELION_OP_STORE_LOCAL,
                          slot, 0, location)) return false;
        return true;
    case AST_STMT_EXPRESSION:
        if (!compile_expr(state, statement->as.expression.expression)) return false;
        if (state->options->echo_expressions && state->current->entry &&
            state->entry_top_level && statement->as.expression.expression != NULL &&
            statement->as.expression.expression->inferred_type != TYPE_SILENT &&
            !emit_checked(state, APHELION_OP_REVEAL, 0, 0, location)) return false;
        /* REVEAL consumes the value and leaves silent, which also needs popping. */
        return emit_checked(state, APHELION_OP_POP, 0, 0, location);
    case AST_STMT_BLOCK:
        return compile_block(state, statement->as.block.block);
    case AST_STMT_TRAVERSE:
        return compile_traverse(state, statement);
    case AST_STMT_YIELD:
        if ((statement->as.yield.expression != NULL &&
              !compile_as(state, statement->as.yield.expression,
                            state->current->ast->return_type.kind)) ||
            (statement->as.yield.expression == NULL &&
             !emit_checked(state, APHELION_OP_SILENT, 0, 0, location)) ||
            !emit_checked(state, APHELION_OP_RETURN, 0, 0, location)) return false;
        return true;
    case AST_STMT_WHEN:
        if (!compile_expr(state, statement->as.when_stmt.condition)) return false;
        emitted_jump = emit_instruction(state, APHELION_OP_JUMP_IF_FALSE, 0, 0,
                                        location);
        if (emitted_jump < 0 ||
            !emit_checked(state, APHELION_OP_POP, 0, 0, location) ||
            !compile_block(state, statement->as.when_stmt.then_branch)) return false;
        jump_false = (size_t)emitted_jump;
        emitted_jump = emit_instruction(state, APHELION_OP_JUMP, 0, 0, location);
        if (emitted_jump < 0) return false;
        jump_end = (size_t)emitted_jump;
        function = &state->bytecode->functions[state->current->bytecode_index];
        (void)aphelion_function_patch_operand(function, jump_false,
                                               (int32_t)function->code_count);
        if (!emit_checked(state, APHELION_OP_POP, 0, 0, location) ||
            !compile_block(state, statement->as.when_stmt.else_branch)) return false;
        function = &state->bytecode->functions[state->current->bytecode_index];
        (void)aphelion_function_patch_operand(function, jump_end,
                                               (int32_t)function->code_count);
        return true;
    case AST_STMT_CYCLE:
        loop_start = state->bytecode->functions[state->current->bytecode_index].code_count;
        if (!compile_expr(state, statement->as.cycle.condition)) return false;
        emitted_jump = emit_instruction(state, APHELION_OP_JUMP_IF_FALSE, 0, 0,
                                        location);
        if (emitted_jump < 0 ||
            !emit_checked(state, APHELION_OP_POP, 0, 0, location)) return false;
        jump_false = (size_t)emitted_jump;
        memset(&loop, 0, sizeof(loop));
        loop.continue_target = loop_start;
        loop.parent = state->current->loop;
        state->current->loop = &loop;
        if (!compile_block(state, statement->as.cycle.body)) {
            state->current->loop = loop.parent;
            free(loop.break_jumps);
            return false;
        }
        if (!emit_checked(state, APHELION_OP_JUMP, (int32_t)loop_start, 0, location)) {
            state->current->loop = loop.parent;
            free(loop.break_jumps);
            return false;
        }
        function = &state->bytecode->functions[state->current->bytecode_index];
        (void)aphelion_function_patch_operand(function, jump_false,
                                               (int32_t)function->code_count);
        /* A false condition remains on the stack because conditional jumps
         * only peek.  Breaks already consumed their true condition, so they
         * skip this cleanup instruction. */
        if (!emit_checked(state, APHELION_OP_POP, 0, 0, location)) {
            state->current->loop = loop.parent;
            free(loop.break_jumps);
            return false;
        }
        function = &state->bytecode->functions[state->current->bytecode_index];
        for (i = 0u; i < loop.break_count; ++i) {
            (void)aphelion_function_patch_operand(function, loop.break_jumps[i],
                                                   (int32_t)function->code_count);
        }
        state->current->loop = loop.parent;
        free(loop.break_jumps);
        return true;
    case AST_STMT_ESCAPE:
        if (state->current->loop == NULL) {
            compile_error_at(state, location, "escape is only valid inside cycle or traverse");
            return false;
        }
        emitted_jump = emit_instruction(state, APHELION_OP_JUMP, 0, 0, location);
        if (emitted_jump < 0) return false;
        jump_end = (size_t)emitted_jump;
        return add_break_jump(state, state->current->loop, jump_end);
    case AST_STMT_SKIP:
        if (state->current->loop == NULL) {
            compile_error_at(state, location, "skip is only valid inside cycle or traverse");
            return false;
        }
        return emit_checked(state, APHELION_OP_JUMP,
                            (int32_t)state->current->loop->continue_target,
                            0, location);
    default:
        compile_error_at(state, location, "unsupported statement kind");
        return false;
    }
}

static bool compile_function_body(CompilerState *state, CompileFunction *function) {
    AphelionSourceLocation location = function_location(state, function->ast);
    CompileFunction *saved = state->current;
    state->current = function;
    if (!emit_source(state, location) || !compile_block(state, function->ast->body)) {
        state->current = saved;
        return false;
    }
    if (!emit_checked(state,
                      function->ast->return_type.kind == TYPE_SILENT
                        ? APHELION_OP_SILENT : APHELION_OP_HOLLOW,
                      0, 0, location) ||
        !emit_checked(state, APHELION_OP_RETURN, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_HALT, 0, 0, location)) {
        state->current = saved;
        return false;
    }
    state->current = saved;
    return true;
}

static bool compile_entry(CompilerState *state) {
    size_t i;
    AphelionSourceLocation location = source_location(state->program, (SourceLocation){
        state->program != NULL ? state->program->filename : "<source>", 0u, 1u, 1u});
    CompileFunction *entry = &state->functions[0];
    CompileFunction *saved = state->current;
    state->current = entry;
    state->entry_top_level = true;
    if (!emit_source(state, location)) {
        state->current = saved;
        return false;
    }
    for (i = 0u; i < state->program->item_count; ++i) {
        const AstTopLevel *item = &state->program->items[i];
        int global;
        if (item->kind == AST_TOP_FUNCTION) {
            CompileFunction *function = find_function_context(state,
                                                               item->as.function->name);
            global = find_global(state, item->as.function->name);
            if (function == NULL || global < 0 ||
                !emit_source(state, source_location(state->program, item->span.start)) ||
                !emit_checked(state, APHELION_OP_MAKE_FUNCTION,
                              function->bytecode_index, 0,
                              source_location(state->program, item->span.start)) ||
                !emit_checked(state, APHELION_OP_STORE_GLOBAL, global, 0,
                              source_location(state->program, item->span.start))) {
                state->current = saved;
                return false;
            }
        }
    }
    for (i = 0u; i < state->program->item_count; ++i) {
        const AstTopLevel *item = &state->program->items[i];
        if (item->kind == AST_TOP_FUNCTION) continue;
        state->entry_top_level = true;
        if (item->as.statement == NULL ||
            !emit_source(state, source_location(state->program, item->span.start)) ||
            !compile_stmt(state, item->as.statement)) {
            state->current = saved;
            return false;
        }
    }
    if (!emit_source(state, location) ||
        !emit_checked(state, APHELION_OP_HOLLOW, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_RETURN, 0, 0, location) ||
        !emit_checked(state, APHELION_OP_HALT, 0, 0, location)) {
        state->current = saved;
        return false;
    }
    state->entry_top_level = false;
    state->current = saved;
    return true;
}

bool aphelion_compile_program(const AstProgram *program,
                              const AphelionCompilerOptions *options,
                              AphelionBytecode *out,
                              AphelionDiagnostics *diagnostics) {
    AphelionCompilerOptions defaults;
    CompilerState state;
    size_t i;
    size_t function_number = 1u;
    bool ok = true;

    if (program == NULL || out == NULL || diagnostics == NULL ||
        program->had_parse_error) {
        if (diagnostics != NULL) {
            (void)aphelion_diagnostics_add(diagnostics, APHELION_DIAGNOSTIC_ERROR,
                                           (AphelionSourceLocation){
                                             program != NULL ? program->filename : "<source>",
                                             1u, 1u},
                                           program != NULL && program->had_parse_error
                                             ? "cannot compile a program with parse errors"
                                             : "invalid compiler arguments");
        }
        return false;
    }
    if (options == NULL) {
        aphelion_compiler_options_default(&defaults);
        options = &defaults;
    }
    aphelion_bytecode_destroy(out);
    aphelion_bytecode_init(out);
    if (!aphelion_bytecode_set_source_name(out, program->filename)) {
        (void)aphelion_diagnostics_add(diagnostics, APHELION_DIAGNOSTIC_ERROR,
                                       (AphelionSourceLocation){program->filename, 1u, 1u},
                                       "out of memory setting source name");
        return false;
    }
    memset(&state, 0, sizeof(state));
    state.program = program;
    state.options = options;
    state.bytecode = out;
    state.diagnostics = diagnostics;
    state.function_count = 1u;
    for (i = 0u; i < program->item_count; ++i) {
        const AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_FUNCTION) {
            if (function_number >= options->max_functions) {
                compile_error_at(&state,
                                 source_location(program, item->span.start),
                                 "function limit exceeded");
                ok = false;
            }
            state.function_count += 1u;
            function_number += 1u;
        }
    }
    state.functions = (CompileFunction *)calloc(state.function_count,
                                                 sizeof(*state.functions));
    if (state.functions == NULL) {
        compile_error_at(&state, (AphelionSourceLocation){program->filename, 1u, 1u},
                         "out of memory creating compiler state");
        return false;
    }
    state.functions[0].entry = true;
    state.functions[0].bytecode_index = aphelion_bytecode_add_function(out, "main", 0u, 0u);
    if (state.functions[0].bytecode_index < 0) {
        compile_error_at(&state, (AphelionSourceLocation){program->filename, 1u, 1u},
                         "out of memory creating entry function");
        free_function_contexts(state.functions, state.function_count);
        aphelion_bytecode_destroy(out);
        return false;
    }
    state.function_count = 1u;
    for (i = 0u; i < program->item_count; ++i) {
        const AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_FUNCTION) {
            int index;
            CompileFunction *context;
            if (item->as.function == NULL || item->as.function->name == NULL) {
                compile_error_at(&state, source_location(program, item->span.start),
                                 "function must have a name");
                ok = false;
                continue;
            }
            if (find_function_context(&state, item->as.function->name) != NULL) {
                compile_error_at(&state, function_location(&state, item->as.function),
                                 "duplicate function '%s'", item->as.function->name);
                ok = false;
                continue;
            }
            index = aphelion_bytecode_add_function(out,
                                                   item->as.function->name,
                                                   (uint32_t)item->as.function->parameter_count,
                                                   0u);
            if (index < 0) {
                compile_error_at(&state, function_location(&state, item->as.function),
                                 "out of memory adding function");
                ok = false;
                continue;
            }
            context = &state.functions[state.function_count++];
            context->ast = item->as.function;
            context->bytecode_index = index;
            for (size_t p = 0u; p < item->as.function->parameter_count; ++p) {
                if (add_local(&state, context, item->as.function->parameters[p].name,
                              &item->as.function->parameters[p],
                              source_location(program,
                                item->as.function->parameters[p].span.start)) < 0) {
                    ok = false;
                }
            }
            if (!collect_locals_in_block(&state, context, item->as.function->body)) {
                ok = false;
            }
            out->functions[index].local_count = (uint32_t)context->local_count;
        }
    }
    /* Globals are registered before code generation, so forward references work. */
    for (i = 0u; i < program->item_count; ++i) {
        const AstTopLevel *item = &program->items[i];
        const char *name = NULL;
        AphelionSourceLocation location = source_location(program, item->span.start);
        if (item->kind == AST_TOP_FUNCTION && item->as.function != NULL) {
            name = item->as.function->name;
        } else if (item->kind == AST_TOP_DECLARATION && item->as.statement != NULL &&
                   item->as.statement->kind == AST_STMT_VAR_DECL) {
            name = item->as.statement->as.var_decl.name;
        }
        if (name != NULL && add_global(&state, name, location) < 0) ok = false;
    }
    for (i = 0u; i < program->item_count; ++i) {
        const AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_EXPRESSION &&
            !collect_entry_nested_locals_in_stmt(&state, &state.functions[0],
                                                 item->as.statement)) {
            ok = false;
        }
    }
    out->functions[0].local_count = (uint32_t)state.functions[0].local_count;
    out->entry_function = 0u;
    for (i = 1u; i < state.function_count; ++i) {
        if (!compile_function_body(&state, &state.functions[i])) ok = false;
    }
    if (!compile_entry(&state)) ok = false;
    if (aphelion_diagnostics_has_errors(diagnostics)) ok = false;
    free_function_contexts(state.functions, state.function_count);
    if (!ok) {
        aphelion_bytecode_destroy(out);
    }
    return ok;
}

bool aphelion_compile_ast(const AstProgram *program,
                          const AphelionCompilerOptions *options,
                          AphelionBytecode *out,
                          AphelionDiagnostics *diagnostics) {
    return aphelion_compile_program(program, options, out, diagnostics);
}
