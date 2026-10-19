#include "aphelion/ast.h"

#include <stdlib.h>
#include <string.h>

static char *ast_strdup(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1u);
    return copy;
}

static bool grow_array(void **data, size_t *capacity, size_t count,
                       size_t element_size) {
    size_t new_capacity;
    void *new_data;

    if (*capacity >= count) {
        return true;
    }
    new_capacity = (*capacity == 0u) ? 4u : *capacity;
    while (new_capacity < count) {
        if (new_capacity > (SIZE_MAX / 2u)) {
            new_capacity = count;
            break;
        }
        new_capacity *= 2u;
    }
    if (element_size != 0u && new_capacity > SIZE_MAX / element_size) {
        return false;
    }
    new_data = realloc(*data, new_capacity * element_size);
    if (new_data == NULL) {
        return false;
    }
    *data = new_data;
    *capacity = new_capacity;
    return true;
}

static AstType empty_type(void) {
    AstType type;
    type.kind = TYPE_UNKNOWN;
    type.name = NULL;
    type.explicitly_typed = false;
    return type;
}

AstProgram *ast_program_new(const char *filename) {
    AstProgram *program = (AstProgram *)calloc(1u, sizeof(*program));
    if (program == NULL) {
        return NULL;
    }
    program->filename = ast_strdup(filename != NULL ? filename : "<source>");
    if (program->filename == NULL) {
        free(program);
        return NULL;
    }
    return program;
}

AstFunction *ast_function_new(SourceSpan span, const char *name) {
    AstFunction *function = (AstFunction *)calloc(1u, sizeof(*function));
    if (function == NULL) {
        return NULL;
    }
    function->span = span;
    function->name = ast_strdup(name);
    if (name != NULL && function->name == NULL) {
        free(function);
        return NULL;
    }
    function->return_type = empty_type();
    return function;
}

AstBlock *ast_block_new(SourceSpan span) {
    AstBlock *block = (AstBlock *)calloc(1u, sizeof(*block));
    if (block != NULL) {
        block->span = span;
    }
    return block;
}

AstStmt *ast_stmt_new(AstStmtKind kind, SourceSpan span) {
    AstStmt *statement = (AstStmt *)calloc(1u, sizeof(*statement));
    if (statement != NULL) {
        statement->kind = kind;
        statement->span = span;
        if (kind == AST_STMT_VAR_DECL) {
            statement->as.var_decl.type = empty_type();
        }
    }
    return statement;
}

AstExpr *ast_expr_new(AstExprKind kind, SourceSpan span) {
    AstExpr *expression = (AstExpr *)calloc(1u, sizeof(*expression));
    if (expression != NULL) {
        expression->kind = kind;
        expression->span = span;
        expression->depth = 1u;
        expression->inferred_type = TYPE_UNKNOWN;
    }
    return expression;
}

bool ast_program_add_item(AstProgram *program, AstTopLevel item) {
    if (program == NULL ||
        !grow_array((void **)&program->items, &program->item_capacity,
                    program->item_count + 1u, sizeof(*program->items))) {
        return false;
    }
    program->items[program->item_count++] = item;
    return true;
}

bool ast_block_add_statement(AstBlock *block, AstStmt *statement) {
    if (block == NULL || statement == NULL ||
        !grow_array((void **)&block->statements, &block->statement_capacity,
                    block->statement_count + 1u, sizeof(*block->statements))) {
        return false;
    }
    block->statements[block->statement_count++] = statement;
    return true;
}

bool ast_function_add_parameter(AstFunction *function, AstParameter parameter) {
    if (function == NULL ||
        !grow_array((void **)&function->parameters,
                    &function->parameter_capacity,
                    function->parameter_count + 1u,
                    sizeof(*function->parameters))) {
        return false;
    }
    function->parameters[function->parameter_count++] = parameter;
    return true;
}

bool ast_call_add_argument(AstExpr *call, AstExpr *argument) {
    if (call == NULL || call->kind != AST_EXPR_CALL || argument == NULL ||
        !grow_array((void **)&call->as.call.arguments,
                    &call->as.call.argument_capacity,
                    call->as.call.argument_count + 1u,
                    sizeof(*call->as.call.arguments))) {
        return false;
    }
    call->as.call.arguments[call->as.call.argument_count++] = argument;
    return true;
}

bool ast_list_add_element(AstExpr *list, AstExpr *element) {
    if (list == NULL || list->kind != AST_EXPR_LIST || element == NULL ||
        list->as.list.element_count == SIZE_MAX ||
        !grow_array((void **)&list->as.list.elements,
                    &list->as.list.element_capacity,
                    list->as.list.element_count + 1u,
                    sizeof(*list->as.list.elements))) {
        return false;
    }
    list->as.list.elements[list->as.list.element_count++] = element;
    return true;
}

static void free_type(AstType *type) {
    if (type == NULL) {
        return;
    }
    free(type->name);
    type->name = NULL;
}

static void free_expr(AstExpr *expression);
static void free_stmt(AstStmt *statement);

static void free_block(AstBlock *block) {
    size_t i;
    if (block == NULL) {
        return;
    }
    for (i = 0u; i < block->statement_count; ++i) {
        free_stmt(block->statements[i]);
    }
    free(block->statements);
    free(block);
}

static void free_expr(AstExpr *expression) {
    size_t i;
    if (expression == NULL) {
        return;
    }
    switch (expression->kind) {
    case AST_EXPR_LITERAL:
        free(expression->as.literal.text);
        break;
    case AST_EXPR_NAME:
        free(expression->as.name.name);
        break;
    case AST_EXPR_UNARY:
        free_expr(expression->as.unary.operand);
        break;
    case AST_EXPR_BINARY:
        free_expr(expression->as.binary.left);
        free_expr(expression->as.binary.right);
        break;
    case AST_EXPR_CALL:
        free_expr(expression->as.call.callee);
        for (i = 0u; i < expression->as.call.argument_count; ++i) {
            free_expr(expression->as.call.arguments[i]);
        }
        free(expression->as.call.arguments);
        break;
    case AST_EXPR_ASSIGN:
        free_expr(expression->as.assignment.target);
        free_expr(expression->as.assignment.value);
        break;
    case AST_EXPR_GROUP:
        free_expr(expression->as.group.inner);
        break;
    case AST_EXPR_LIST:
        for (i = 0u; i < expression->as.list.element_count; ++i) {
            free_expr(expression->as.list.elements[i]);
        }
        free(expression->as.list.elements);
        break;
    case AST_EXPR_INDEX:
        free_expr(expression->as.index.target);
        free_expr(expression->as.index.index);
        break;
    }
    free(expression);
}

static void free_stmt(AstStmt *statement) {
    if (statement == NULL) {
        return;
    }
    switch (statement->kind) {
    case AST_STMT_VAR_DECL:
        free(statement->as.var_decl.name);
        free_type(&statement->as.var_decl.type);
        free_expr(statement->as.var_decl.initializer);
        break;
    case AST_STMT_EXPRESSION:
        free_expr(statement->as.expression.expression);
        break;
    case AST_STMT_BLOCK:
        free_block(statement->as.block.block);
        break;
    case AST_STMT_YIELD:
        free_expr(statement->as.yield.expression);
        break;
    case AST_STMT_WHEN:
        free_expr(statement->as.when_stmt.condition);
        free_block(statement->as.when_stmt.then_branch);
        free_block(statement->as.when_stmt.else_branch);
        break;
    case AST_STMT_CYCLE:
        free_expr(statement->as.cycle.condition);
        free_block(statement->as.cycle.body);
        break;
    case AST_STMT_TRAVERSE:
        free_stmt(statement->as.traverse.binding);
        free_expr(statement->as.traverse.iterable);
        free_block(statement->as.traverse.body);
        break;
    case AST_STMT_ESCAPE:
    case AST_STMT_SKIP:
        break;
    case AST_STMT_SUMMON:
        free(statement->as.summon.path);
        break;
    }
    free(statement);
}

static void free_function(AstFunction *function) {
    size_t i;
    if (function == NULL) {
        return;
    }
    free(function->name);
    for (i = 0u; i < function->parameter_count; ++i) {
        free(function->parameters[i].name);
        free_type(&function->parameters[i].type);
    }
    free(function->parameters);
    free_type(&function->return_type);
    free_block(function->body);
    free(function);
}

void ast_program_free(AstProgram *program) {
    size_t i;
    if (program == NULL) {
        return;
    }
    for (i = 0u; i < program->item_count; ++i) {
        if (program->items[i].kind == AST_TOP_FUNCTION) {
            free_function(program->items[i].as.function);
        } else {
            free_stmt(program->items[i].as.statement);
        }
    }
    free(program->items);
    for (i = 0u; i < program->source_file_count; ++i) {
        free(program->source_files[i]);
    }
    free(program->source_files);
    free(program->filename);
    free(program);
}

void ast_expr_free(AstExpr *expression) { free_expr(expression); }
void ast_stmt_free(AstStmt *statement) { free_stmt(statement); }
void ast_function_free(AstFunction *function) { free_function(function); }

const char *type_kind_name(TypeKind type) {
    switch (type) {
    case TYPE_WHOLE:
        return "whole";
    case TYPE_DECIMAL:
        return "decimal";
    case TYPE_SIGNAL:
        return "signal";
    case TYPE_TEXT:
        return "text";
    case TYPE_GLYPH:
        return "glyph";
    case TYPE_SILENT:
        return "silent";
    case TYPE_HOLLOW:
        return "hollow";
    case TYPE_LIST_WHOLE: return "[whole]";
    case TYPE_LIST_DECIMAL: return "[decimal]";
    case TYPE_LIST_SIGNAL: return "[signal]";
    case TYPE_LIST_TEXT: return "[text]";
    case TYPE_LIST_GLYPH: return "[glyph]";
    case TYPE_LIST_EMPTY: return "[]";
    case TYPE_ERROR:
        return "<error>";
    case TYPE_UNKNOWN:
    default:
        return "<unknown>";
    }
}

bool type_kind_is_numeric(TypeKind type) {
    return type == TYPE_WHOLE || type == TYPE_DECIMAL;
}

bool type_kind_is_value(TypeKind type) {
    return (type >= TYPE_WHOLE && type <= TYPE_HOLLOW) || type_kind_is_list(type);
}

bool type_kind_is_list(TypeKind type) {
    return type >= TYPE_LIST_WHOLE && type <= TYPE_LIST_EMPTY;
}

TypeKind type_kind_list_of(TypeKind element) {
    switch (element) {
    case TYPE_WHOLE: return TYPE_LIST_WHOLE;
    case TYPE_DECIMAL: return TYPE_LIST_DECIMAL;
    case TYPE_SIGNAL: return TYPE_LIST_SIGNAL;
    case TYPE_TEXT: return TYPE_LIST_TEXT;
    case TYPE_GLYPH: return TYPE_LIST_GLYPH;
    default: return TYPE_UNKNOWN;
    }
}

TypeKind type_kind_list_element(TypeKind list) {
    switch (list) {
    case TYPE_LIST_WHOLE: return TYPE_WHOLE;
    case TYPE_LIST_DECIMAL: return TYPE_DECIMAL;
    case TYPE_LIST_SIGNAL: return TYPE_SIGNAL;
    case TYPE_LIST_TEXT: return TYPE_TEXT;
    case TYPE_LIST_GLYPH: return TYPE_GLYPH;
    default: return TYPE_UNKNOWN;
    }
}
