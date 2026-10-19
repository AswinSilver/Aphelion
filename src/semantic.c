#include "aphelion/semantic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum SymbolKind {
    SYMBOL_VARIABLE = 0,
    SYMBOL_PARAMETER,
    SYMBOL_FUNCTION
} SymbolKind;

typedef struct Symbol {
    const char *name;       /* borrowed from the AST */
    SymbolKind kind;
    TypeKind type;
    bool mutable;
    bool defined;
    union {
        AstStmt *declaration;
        AstParameter *parameter;
        AstFunction *function;
    } as;
    struct Symbol *next;
} Symbol;

typedef struct Scope {
    struct Scope *parent;
    Symbol *symbols;
} Scope;

typedef struct FunctionContext {
    AstFunction *function;
    bool saw_yield;
    unsigned loop_depth;
} FunctionContext;

typedef struct SemanticContext {
    Diagnostics *diagnostics;
    bool ok;
    Scope *global;
} SemanticContext;

static void semantic_error_span(SemanticContext *context, SourceSpan span,
                                const char *format, ...) {
    char message[768];
    va_list arguments;

    context->ok = false;
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (context->diagnostics != NULL) {
        (void)diagnostics_add(context->diagnostics, span.start, message);
    }
}

static Symbol *scope_find_local(Scope *scope, const char *name) {
    Symbol *symbol;
    if (scope == NULL) {
        return NULL;
    }
    if (name == NULL) {
        return NULL;
    }
    for (symbol = scope->symbols; symbol != NULL; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

static Symbol *scope_lookup(Scope *scope, const char *name) {
    Symbol *symbol;
    for (; scope != NULL; scope = scope->parent) {
        symbol = scope_find_local(scope, name);
        if (symbol != NULL) {
            return symbol;
        }
    }
    return NULL;
}

static Symbol *scope_declare(SemanticContext *context, Scope *scope,
                             const char *name, SymbolKind kind, TypeKind type,
                             bool mutable, void *node, SourceSpan span) {
    Symbol *symbol;
    if (scope == NULL) {
        return NULL;
    }
    if (name == NULL) {
        semantic_error_span(context, span, "declaration has no name");
        return NULL;
    }
    if (scope_find_local(scope, name) != NULL) {
        semantic_error_span(context, span, "duplicate name '%s' in this scope",
                            name);
        return NULL;
    }
    if (strcmp(name, "to_text") == 0 || strcmp(name, "to_whole") == 0 ||
        strcmp(name, "to_decimal") == 0 || strcmp(name, "length") == 0 ||
        strcmp(name, "kind") == 0 || strcmp(name, "assert") == 0) {
        semantic_error_span(context, span, "builtin name '%s' cannot be redeclared", name);
        return NULL;
    }
    symbol = (Symbol *)calloc(1u, sizeof(*symbol));
    if (symbol == NULL) {
        semantic_error_span(context, span,
                            "out of memory while creating symbol '%s'", name);
        return NULL;
    }
    symbol->name = name;
    symbol->kind = kind;
    symbol->type = type;
    symbol->mutable = mutable;
    symbol->defined = false;
    if (kind == SYMBOL_FUNCTION) {
        symbol->as.function = (AstFunction *)node;
    } else if (kind == SYMBOL_PARAMETER) {
        symbol->as.parameter = (AstParameter *)node;
    } else {
        symbol->as.declaration = (AstStmt *)node;
    }
    symbol->next = scope->symbols;
    scope->symbols = symbol;
    return symbol;
}

static void scope_free_symbols(Scope *scope) {
    Symbol *symbol;
    Symbol *next;
    if (scope == NULL) {
        return;
    }
    symbol = scope->symbols;
    while (symbol != NULL) {
        next = symbol->next;
        free(symbol);
        symbol = next;
    }
    scope->symbols = NULL;
}

static TypeKind numeric_result(TypeKind left, TypeKind right) {
    return left == TYPE_DECIMAL || right == TYPE_DECIMAL
               ? TYPE_DECIMAL
               : TYPE_WHOLE;
}

bool semantic_types_compatible(TypeKind expected, TypeKind actual) {
    if (expected == TYPE_ERROR || actual == TYPE_ERROR) {
        return true;
    }
    if (expected == actual) {
        return true;
    }
    if (type_kind_is_list(expected) && expected != TYPE_LIST_EMPTY &&
        actual == TYPE_LIST_EMPTY) {
        return true;
    }
    /* Whole values widen naturally when a decimal is required. */
    if (expected == TYPE_DECIMAL && actual == TYPE_WHOLE) {
        return true;
    }
    return false;
}

static bool is_signal(TypeKind type) {
    return type == TYPE_SIGNAL || type == TYPE_ERROR;
}

static bool is_numeric_or_error(TypeKind type) {
    return type_kind_is_numeric(type) || type == TYPE_ERROR;
}

static bool is_numeric_conversion_input(TypeKind type) {
    return type == TYPE_WHOLE || type == TYPE_DECIMAL ||
           type == TYPE_TEXT || type == TYPE_ERROR;
}

static AstExpr *unwrap_group(AstExpr *expression) {
    while (expression != NULL && expression->kind == AST_EXPR_GROUP) {
        expression = expression->as.group.inner;
    }
    return expression;
}

static bool is_constant_expression_in_scope(Scope *scope,
                                            const AstExpr *expression,
                                            unsigned depth) {
    if (expression == NULL) {
        return false;
    }
    if (depth > 128u) {
        return false;
    }
    switch (expression->kind) {
    case AST_EXPR_LITERAL:
        return true;
    case AST_EXPR_LIST: {
        size_t i;
        for (i = 0u; i < expression->as.list.element_count; ++i) {
            if (!is_constant_expression_in_scope(scope,
                    expression->as.list.elements[i], depth + 1u)) return false;
        }
        return true;
    }
    case AST_EXPR_INDEX:
        return is_constant_expression_in_scope(scope, expression->as.index.target,
                                               depth + 1u) &&
               is_constant_expression_in_scope(scope, expression->as.index.index,
                                               depth + 1u);
    case AST_EXPR_GROUP:
        return is_constant_expression_in_scope(scope, expression->as.group.inner,
                                               depth + 1u);
    case AST_EXPR_UNARY:
        return is_constant_expression_in_scope(scope,
                                               expression->as.unary.operand,
                                               depth + 1u);
    case AST_EXPR_BINARY:
        return is_constant_expression_in_scope(scope, expression->as.binary.left,
                                               depth + 1u) &&
               is_constant_expression_in_scope(scope, expression->as.binary.right,
                                               depth + 1u);
    case AST_EXPR_NAME: {
        Symbol *symbol = scope_lookup(scope, expression->as.name.name);
        if (symbol == NULL || symbol->kind != SYMBOL_VARIABLE ||
            !symbol->defined || symbol->as.declaration == NULL ||
            symbol->as.declaration->as.var_decl.storage != AST_STORAGE_ANCHOR) {
            return false;
        }
        return is_constant_expression_in_scope(
            scope, symbol->as.declaration->as.var_decl.initializer, depth + 1u);
    }
    default:
        return false;
    }
}

static bool block_always_yields(const AstBlock *block);

static bool statement_always_yields(const AstStmt *statement) {
    if (statement == NULL) {
        return false;
    }
    switch (statement->kind) {
    case AST_STMT_YIELD:
        return true;
    case AST_STMT_BLOCK:
        return block_always_yields(statement->as.block.block);
    case AST_STMT_WHEN:
        return statement->as.when_stmt.else_branch != NULL &&
               block_always_yields(statement->as.when_stmt.then_branch) &&
               block_always_yields(statement->as.when_stmt.else_branch);
    default:
        return false;
    }
}

static bool block_always_yields(const AstBlock *block) {
    size_t i;
    if (block == NULL) {
        return false;
    }
    for (i = 0u; i < block->statement_count; ++i) {
        if (statement_always_yields(block->statements[i])) {
            return true;
        }
    }
    return false;
}

static TypeKind check_expression(SemanticContext *context, Scope *scope,
                                 AstExpr *expression);
static void check_statement(SemanticContext *context, Scope *scope,
                            FunctionContext *function,
                            AstStmt *statement);

static TypeKind check_name(SemanticContext *context, Scope *scope,
                           AstExpr *expression) {
    const char *name = expression->as.name.name != NULL
                           ? expression->as.name.name
                           : "<unnamed>";
    Symbol *symbol = scope_lookup(scope, expression->as.name.name);
    if (symbol == NULL) {
        semantic_error_span(context, expression->span,
                            "undefined name '%s'", name);
        expression->inferred_type = TYPE_ERROR;
        return TYPE_ERROR;
    }
    expression->resolved_symbol = symbol->kind == SYMBOL_FUNCTION
                                      ? (void *)symbol->as.function
                                  : symbol->kind == SYMBOL_PARAMETER
                                      ? (void *)symbol->as.parameter
                                      : (void *)symbol->as.declaration;
    if (symbol->kind == SYMBOL_FUNCTION) {
        semantic_error_span(context, expression->span,
                            "function '%s' must be called", symbol->name);
        expression->inferred_type = TYPE_ERROR;
        return TYPE_ERROR;
    }
    if (!symbol->defined) {
        semantic_error_span(context, expression->span,
                            "name '%s' is used before its initializer",
                            symbol->name);
        expression->inferred_type = TYPE_ERROR;
        return TYPE_ERROR;
    }
    if (symbol->type == TYPE_UNKNOWN) {
        semantic_error_span(context, expression->span,
                            "cannot infer the type of '%s' before its initializer",
                            symbol->name);
        expression->inferred_type = TYPE_ERROR;
        return TYPE_ERROR;
    }
    expression->inferred_type = symbol->type;
    return symbol->type;
}

static TypeKind check_call(SemanticContext *context, Scope *scope,
                           AstExpr *expression) {
    AstExpr *callee = unwrap_group(expression->as.call.callee);
    Symbol *symbol = NULL;
    size_t i;
    size_t expected_count = 0u;
    AstParameter *parameters = NULL;
    TypeKind result = TYPE_ERROR;

    if (callee != NULL && callee->kind == AST_EXPR_NAME) {
        if (strcmp(callee->as.name.name, "reveal") == 0) {
            if (expression->as.call.argument_count != 1u) {
                semantic_error_span(context, expression->span,
                                    "reveal expects exactly one argument, got %lu",
                                    (unsigned long)expression->as.call.argument_count);
            }
            for (i = 0u; i < expression->as.call.argument_count; ++i) {
                (void)check_expression(context, scope,
                                        expression->as.call.arguments[i]);
            }
            expression->resolved_symbol = NULL;
            expression->inferred_type = TYPE_SILENT;
            return TYPE_SILENT;
        }
        if (strcmp(callee->as.name.name, "to_text") == 0) {
            if (expression->as.call.argument_count != 1u) {
                semantic_error_span(context, expression->span,
                                    "to_text expects exactly one argument, got %lu",
                                    (unsigned long)expression->as.call.argument_count);
            }
            for (i = 0u; i < expression->as.call.argument_count; ++i) {
                (void)check_expression(context, scope,
                                        expression->as.call.arguments[i]);
            }
            expression->resolved_symbol = NULL;
            expression->inferred_type = TYPE_TEXT;
            return TYPE_TEXT;
        }
        if (strcmp(callee->as.name.name, "receive") == 0) {
            if (expression->as.call.argument_count != 0u) {
                semantic_error_span(context, expression->span,
                                    "receive expects no arguments, got %lu",
                                    (unsigned long)expression->as.call.argument_count);
            }
            expression->resolved_symbol = NULL;
            expression->inferred_type = TYPE_TEXT;
            return TYPE_TEXT;
        }
        if (strcmp(callee->as.name.name, "to_whole") == 0 ||
            strcmp(callee->as.name.name, "to_decimal") == 0) {
            const char *builtin = callee->as.name.name;
            TypeKind target = strcmp(builtin, "to_whole") == 0
                                ? TYPE_WHOLE : TYPE_DECIMAL;
            if (expression->as.call.argument_count != 1u) {
                semantic_error_span(context, expression->span,
                                    "%s expects exactly one argument, got %lu",
                                    builtin,
                                    (unsigned long)expression->as.call.argument_count);
            }
            for (i = 0u; i < expression->as.call.argument_count; ++i) {
                TypeKind actual = check_expression(context, scope,
                                                   expression->as.call.arguments[i]);
                if (i == 0u && !is_numeric_conversion_input(actual)) {
                    semantic_error_span(context, expression->as.call.arguments[i]->span,
                                        "%s accepts whole, decimal, or text; got %s",
                                        builtin, type_kind_name(actual));
                }
            }
            expression->resolved_symbol = NULL;
            expression->inferred_type = target;
            return target;
        }
        if (strcmp(callee->as.name.name, "length") == 0 ||
            strcmp(callee->as.name.name, "kind") == 0 ||
            strcmp(callee->as.name.name, "assert") == 0) {
            const char *builtin = callee->as.name.name;
            bool assertion = strcmp(builtin, "assert") == 0;
            bool length = strcmp(builtin, "length") == 0;
            size_t count = expression->as.call.argument_count;
            TypeKind target = assertion ? TYPE_SILENT
                                       : length ? TYPE_WHOLE : TYPE_TEXT;
            if (assertion ? (count != 1u && count != 2u) : count != 1u) {
                semantic_error_span(context, expression->span,
                                    "%s expects %s, got %lu",
                                    builtin,
                                    assertion ? "one or two arguments"
                                              : "exactly one argument",
                                    (unsigned long)count);
            }
            for (i = 0u; i < count; ++i) {
                TypeKind actual = check_expression(context, scope,
                                                   expression->as.call.arguments[i]);
                TypeKind expected = assertion && i == 0u ? TYPE_SIGNAL : TYPE_TEXT;
                if (length && i == 0u) {
                    if (actual != TYPE_ERROR && actual != TYPE_TEXT &&
                        !type_kind_is_list(actual)) {
                        semantic_error_span(context, expression->as.call.arguments[i]->span,
                                            "argument 1 to 'length' has type %s; expected text or list",
                                            type_kind_name(actual));
                    }
                } else if (assertion && i < 2u) {
                    if (actual != TYPE_ERROR && actual != expected) {
                        semantic_error_span(context, expression->as.call.arguments[i]->span,
                                            "argument %lu to '%s' has type %s; expected %s",
                                            (unsigned long)(i + 1u), builtin,
                                            type_kind_name(actual), type_kind_name(expected));
                    }
                } else if (!assertion && !length && i == 0u &&
                           actual != TYPE_ERROR && !type_kind_is_value(actual)) {
                    semantic_error_span(context, expression->as.call.arguments[i]->span,
                                        "kind requires an observable value or silent, got %s",
                                        type_kind_name(actual));
                }
            }
            expression->resolved_symbol = NULL;
            expression->inferred_type = target;
            return target;
        }
        symbol = scope_lookup(scope, callee->as.name.name);
        if (symbol == NULL) {
            semantic_error_span(context, callee->span,
                                "undefined function '%s'", callee->as.name.name);
        } else if (symbol->kind != SYMBOL_FUNCTION) {
            semantic_error_span(context, callee->span,
                                "name '%s' is not callable", callee->as.name.name);
        } else {
            callee->resolved_symbol = (void *)symbol->as.function;
            expression->resolved_symbol = (void *)symbol->as.function;
            expected_count = symbol->as.function->parameter_count;
            parameters = symbol->as.function->parameters;
            result = symbol->as.function->return_type.kind;
            if (result == TYPE_UNKNOWN) {
                result = TYPE_ERROR;
            }
        }
    } else {
        (void)check_expression(context, scope, expression->as.call.callee);
        semantic_error_span(context, expression->span,
                            "call target is not a function name");
    }

    if (symbol != NULL && symbol->kind == SYMBOL_FUNCTION &&
        expression->as.call.argument_count != expected_count) {
        semantic_error_span(context, expression->span,
                            "function '%s' expects %lu argument%s, got %lu",
                            symbol->name, (unsigned long)expected_count,
                            expected_count == 1u ? "" : "s",
                            (unsigned long)expression->as.call.argument_count);
    }
    for (i = 0u; i < expression->as.call.argument_count; ++i) {
        TypeKind actual = check_expression(context, scope,
                                           expression->as.call.arguments[i]);
        if (symbol != NULL && symbol->kind == SYMBOL_FUNCTION &&
            i < expected_count) {
            TypeKind expected = parameters[i].type.kind;
            if (expected == TYPE_UNKNOWN) {
                semantic_error_span(context, parameters[i].span,
                                    "parameter '%s' has no type",
                                    parameters[i].name);
            } else if (!semantic_types_compatible(expected, actual)) {
                semantic_error_span(context, expression->as.call.arguments[i]->span,
                                    "argument %lu to '%s' has type %s; expected %s",
                                    (unsigned long)(i + 1u), symbol->name,
                                    type_kind_name(actual),
                                    type_kind_name(expected));
            }
        }
    }
    expression->inferred_type = result;
    return result;
}

static TypeKind check_expression(SemanticContext *context, Scope *scope,
                                 AstExpr *expression) {
    TypeKind left;
    TypeKind right;
    TypeKind result = TYPE_ERROR;
    if (expression == NULL) {
        return TYPE_ERROR;
    }
    switch (expression->kind) {
    case AST_EXPR_LIST: {
        size_t i;
        TypeKind element = TYPE_UNKNOWN;
        bool valid = true;
        for (i = 0u; i < expression->as.list.element_count; ++i) {
            TypeKind actual = check_expression(context, scope,
                                               expression->as.list.elements[i]);
            if (actual == TYPE_ERROR) {
                valid = false;
            } else if (type_kind_list_of(actual) == TYPE_UNKNOWN) {
                semantic_error_span(context, expression->as.list.elements[i]->span,
                                    "list elements must be scalar whole, decimal, signal, text, or glyph; got %s",
                                    type_kind_name(actual));
                valid = false;
            } else if (element == TYPE_UNKNOWN) {
                element = actual;
            } else if (actual != element) {
                semantic_error_span(context, expression->as.list.elements[i]->span,
                                    "list elements must have exactly one type; got %s and %s",
                                    type_kind_name(element), type_kind_name(actual));
                valid = false;
            }
        }
        result = !valid ? TYPE_ERROR : element == TYPE_UNKNOWN
                   ? TYPE_LIST_EMPTY : type_kind_list_of(element);
        break;
    }
    case AST_EXPR_INDEX:
        left = check_expression(context, scope, expression->as.index.target);
        right = check_expression(context, scope, expression->as.index.index);
        if (left == TYPE_LIST_EMPTY) {
            semantic_error_span(context, expression->span,
                                "cannot index an untyped empty list; a list element type is required");
        } else if (left != TYPE_ERROR && !type_kind_is_list(left)) {
            semantic_error_span(context, expression->as.index.target->span,
                                "index target must be a list, got %s", type_kind_name(left));
        }
        if (right != TYPE_ERROR && right != TYPE_WHOLE) {
            semantic_error_span(context, expression->as.index.index->span,
                                "list index must be whole, got %s", type_kind_name(right));
        }
        result = type_kind_list_element(left);
        if (result == TYPE_UNKNOWN || right != TYPE_WHOLE) result = TYPE_ERROR;
        break;
    case AST_EXPR_LITERAL:
        switch (expression->as.literal.kind) {
        case AST_LITERAL_WHOLE:
            result = TYPE_WHOLE;
            break;
        case AST_LITERAL_DECIMAL:
            result = TYPE_DECIMAL;
            break;
        case AST_LITERAL_SIGNAL:
            result = TYPE_SIGNAL;
            break;
        case AST_LITERAL_TEXT:
            result = TYPE_TEXT;
            break;
        case AST_LITERAL_GLYPH:
            result = TYPE_GLYPH;
            break;
        case AST_LITERAL_HOLLOW:
            result = TYPE_HOLLOW;
            break;
        }
        break;
    case AST_EXPR_NAME:
        return check_name(context, scope, expression);
    case AST_EXPR_GROUP:
        result = check_expression(context, scope, expression->as.group.inner);
        break;
    case AST_EXPR_CALL:
        return check_call(context, scope, expression);
    case AST_EXPR_UNARY:
        left = check_expression(context, scope, expression->as.unary.operand);
        if (expression->as.unary.op == AST_UNARY_NOT) {
            if (!is_signal(left)) {
                semantic_error_span(context, expression->as.unary.operand != NULL
                                          ? expression->as.unary.operand->span
                                          : expression->span,
                                    "operator 'not' requires signal, got %s",
                                    type_kind_name(left));
            }
            result = TYPE_SIGNAL;
        } else {
            if (!is_numeric_or_error(left)) {
                semantic_error_span(context, expression->as.unary.operand != NULL
                                          ? expression->as.unary.operand->span
                                          : expression->span,
                                    "unary sign requires a numeric operand, got %s",
                                    type_kind_name(left));
            }
            result = left;
        }
        break;
    case AST_EXPR_BINARY:
        left = check_expression(context, scope, expression->as.binary.left);
        right = check_expression(context, scope, expression->as.binary.right);
        switch (expression->as.binary.op) {
        case AST_BINARY_ADD:
            if ((left == TYPE_TEXT || left == TYPE_ERROR) &&
                (right == TYPE_TEXT || right == TYPE_ERROR)) {
                result = TYPE_TEXT;
                break;
            }
            /* Text concatenation is the one non-numeric arithmetic form. */
            if (left == TYPE_TEXT || right == TYPE_TEXT) {
                semantic_error_span(context, expression->span,
                                    "operator '+' requires two text values or two numeric values, got %s and %s",
                                    type_kind_name(left), type_kind_name(right));
                result = TYPE_ERROR;
                break;
            }
            /* fall through */
        case AST_BINARY_SUBTRACT:
        case AST_BINARY_MULTIPLY:
        case AST_BINARY_DIVIDE:
            if (!is_numeric_or_error(left) || !is_numeric_or_error(right)) {
                semantic_error_span(context, expression->span,
                                    "arithmetic operators require numeric operands, got %s and %s",
                                    type_kind_name(left), type_kind_name(right));
                result = TYPE_ERROR;
            } else {
                result = numeric_result(left, right);
            }
            break;
        case AST_BINARY_REMAINDER:
            if ((left != TYPE_WHOLE && left != TYPE_ERROR) ||
                (right != TYPE_WHOLE && right != TYPE_ERROR)) {
                semantic_error_span(context, expression->span,
                                    "remainder requires whole operands, got %s and %s",
                                    type_kind_name(left), type_kind_name(right));
                result = TYPE_ERROR;
            } else {
                result = TYPE_WHOLE;
            }
            break;
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            if (!is_signal(left) || !is_signal(right)) {
                semantic_error_span(context, expression->span,
                                    "logical operators require signal operands, got %s and %s",
                                    type_kind_name(left), type_kind_name(right));
            }
            result = TYPE_SIGNAL;
            break;
        case AST_BINARY_EQUAL:
        case AST_BINARY_NOT_EQUAL:
            if (!semantic_types_compatible(left, right) &&
                !semantic_types_compatible(right, left)) {
                semantic_error_span(context, expression->span,
                                    "cannot compare values of type %s and %s",
                                    type_kind_name(left), type_kind_name(right));
            }
            result = TYPE_SIGNAL;
            break;
        case AST_BINARY_LESS:
        case AST_BINARY_LESS_EQUAL:
        case AST_BINARY_GREATER:
        case AST_BINARY_GREATER_EQUAL:
            if (left != TYPE_ERROR && right != TYPE_ERROR &&
                !((type_kind_is_numeric(left) && type_kind_is_numeric(right)) ||
                  (left == TYPE_TEXT && right == TYPE_TEXT) ||
                  (left == TYPE_GLYPH && right == TYPE_GLYPH))) {
                semantic_error_span(context, expression->span,
                                    "ordered comparisons require numeric operands, or matching text/glyph operands, got %s and %s",
                                    type_kind_name(left), type_kind_name(right));
            }
            result = TYPE_SIGNAL;
            break;
        }
        break;
    case AST_EXPR_ASSIGN: {
        AstExpr *target = expression->as.assignment.target;
        TypeKind value_type = check_expression(context, scope,
                                               expression->as.assignment.value);
        Symbol *symbol = NULL;
        if (target == NULL || target->kind != AST_EXPR_NAME) {
            AstExpr *unwrapped = unwrap_group(target);
            semantic_error_span(context, expression->as.assignment.target != NULL
                                      ? expression->as.assignment.target->span
                                      : expression->span,
                                unwrapped != NULL && unwrapped->kind == AST_EXPR_INDEX
                                    ? "lists are immutable; indexed assignment is not allowed"
                                    : "assignment target must be a variable name");
            result = TYPE_ERROR;
            break;
        }
        symbol = scope_lookup(scope, target->as.name.name);
        if (symbol == NULL) {
            semantic_error_span(context, target->span,
                                "undefined name '%s'", target->as.name.name);
            result = TYPE_ERROR;
            break;
        }
        target->resolved_symbol = symbol->kind == SYMBOL_PARAMETER
                                      ? (void *)symbol->as.parameter
                                  : symbol->kind == SYMBOL_FUNCTION
                                      ? (void *)symbol->as.function
                                      : (void *)symbol->as.declaration;
        target->inferred_type = symbol->type;
        if (symbol->kind == SYMBOL_FUNCTION) {
            semantic_error_span(context, target->span,
                                "function '%s' cannot be assigned", symbol->name);
            result = TYPE_ERROR;
            break;
        }
        if (!symbol->mutable) {
            semantic_error_span(context, target->span,
                                "cannot assign to immutable name '%s'",
                                symbol->name);
        }
        if (!symbol->defined) {
            semantic_error_span(context, target->span,
                                "cannot assign to '%s' before its initializer", symbol->name);
        }
        if (symbol->type == TYPE_UNKNOWN) {
            semantic_error_span(context, target->span,
                                "cannot assign before '%s' has a type",
                                symbol->name);
            result = TYPE_ERROR;
            break;
        }
        if (expression->as.assignment.op != AST_ASSIGN_SET &&
            (!type_kind_is_numeric(symbol->type) ||
             !type_kind_is_numeric(value_type))) {
            semantic_error_span(context, expression->span,
                                "compound assignment requires numeric operands");
        } else if (!semantic_types_compatible(symbol->type, value_type)) {
            semantic_error_span(context, expression->as.assignment.value != NULL
                                      ? expression->as.assignment.value->span
                                      : expression->span,
                                "cannot assign %s to %s '%s'",
                                type_kind_name(value_type),
                                type_kind_name(symbol->type), symbol->name);
        }
        expression->resolved_symbol = target->resolved_symbol;
        result = symbol->type;
        break;
    }
    }
    expression->inferred_type = result;
    return result;
}

static void check_declaration(SemanticContext *context, Scope *scope,
                              AstStmt *statement, Symbol *symbol) {
    TypeKind actual;
    TypeKind declared = statement->as.var_decl.type.kind;
    AstExpr *initializer = statement->as.var_decl.initializer;

    if (symbol == NULL) {
        return;
    }
    if (initializer == NULL) {
        semantic_error_span(context, statement->span,
                            "declaration '%s' needs an initializer",
                            statement->as.var_decl.name);
        return;
    }
    actual = check_expression(context, scope, initializer);
    if (declared == TYPE_UNKNOWN && actual == TYPE_LIST_EMPTY) {
        semantic_error_span(context, initializer->span,
                            "empty list binding '%s' requires a list type annotation",
                            statement->as.var_decl.name);
        actual = TYPE_ERROR;
    }
    if (declared == TYPE_SILENT || actual == TYPE_SILENT) {
        semantic_error_span(context, statement->span, "silent is not a binding value type");
    }
    if (statement->as.var_decl.storage == AST_STORAGE_ANCHOR &&
        !is_constant_expression_in_scope(scope, initializer, 0u)) {
        semantic_error_span(context, initializer->span,
                            "anchor initializer must be a compile-time constant expression");
    }
    if (declared != TYPE_UNKNOWN) {
        if (!semantic_types_compatible(declared, actual)) {
            semantic_error_span(context, initializer->span,
                                "initializer for '%s' has type %s; expected %s",
                                statement->as.var_decl.name,
                                type_kind_name(actual), type_kind_name(declared));
        }
        symbol->type = declared;
    } else if (actual == TYPE_UNKNOWN || actual == TYPE_ERROR) {
        symbol->type = TYPE_ERROR;
    } else {
        symbol->type = actual;
    }
    symbol->defined = true;
}

static void check_block(SemanticContext *context, Scope *parent,
                        FunctionContext *function, AstBlock *block) {
    Scope scope;
    size_t i;
    if (block == NULL) {
        return;
    }
    memset(&scope, 0, sizeof(scope));
    scope.parent = parent;
    for (i = 0u; i < block->statement_count; ++i) {
        check_statement(context, &scope, function, block->statements[i]);
    }
    scope_free_symbols(&scope);
}

static void check_statement(SemanticContext *context, Scope *scope,
                            FunctionContext *function, AstStmt *statement) {
    TypeKind condition;
    Symbol *symbol;
    if (statement == NULL) {
        return;
    }
    switch (statement->kind) {
    case AST_STMT_VAR_DECL:
        symbol = scope_declare(context, scope, statement->as.var_decl.name,
                               SYMBOL_VARIABLE,
                               statement->as.var_decl.type.kind,
                               statement->as.var_decl.storage == AST_STORAGE_SHIFT,
                               statement, statement->span);
        check_declaration(context, scope, statement, symbol);
        break;
    case AST_STMT_EXPRESSION:
        (void)check_expression(context, scope, statement->as.expression.expression);
        break;
    case AST_STMT_BLOCK:
        check_block(context, scope, function, statement->as.block.block);
        break;
    case AST_STMT_YIELD:
        if (function == NULL || function->function == NULL) {
            semantic_error_span(context, statement->span,
                                "yield is only valid inside an orbit function");
            break;
        }
        function->saw_yield = true;
        if (statement->as.yield.expression == NULL) {
            if (function->function->return_type.kind != TYPE_SILENT) {
                semantic_error_span(context, statement->span,
                                    "orbit '%s' must yield a %s value",
                                    function->function->name,
                                    type_kind_name(function->function->return_type.kind));
            }
        } else {
            TypeKind value = check_expression(context, scope,
                                               statement->as.yield.expression);
            if (function->function->return_type.kind == TYPE_SILENT) {
                semantic_error_span(context, statement->as.yield.expression->span,
                                    "silent orbit cannot yield a value");
            } else if (!semantic_types_compatible(function->function->return_type.kind,
                                                   value)) {
                semantic_error_span(context, statement->as.yield.expression->span,
                                    "yield has type %s; orbit '%s' returns %s",
                                    type_kind_name(value), function->function->name,
                                    type_kind_name(function->function->return_type.kind));
            }
        }
        break;
    case AST_STMT_WHEN:
        condition = check_expression(context, scope,
                                     statement->as.when_stmt.condition);
        if (statement->as.when_stmt.condition != NULL && !is_signal(condition)) {
            semantic_error_span(context, statement->as.when_stmt.condition->span,
                                "when condition must have type signal, got %s",
                                type_kind_name(condition));
        }
        check_block(context, scope, function, statement->as.when_stmt.then_branch);
        check_block(context, scope, function, statement->as.when_stmt.else_branch);
        break;
    case AST_STMT_CYCLE:
        condition = check_expression(context, scope,
                                     statement->as.cycle.condition);
        if (statement->as.cycle.condition != NULL && !is_signal(condition)) {
            semantic_error_span(context, statement->as.cycle.condition->span,
                                "cycle condition must have type signal, got %s",
                                type_kind_name(condition));
        }
        if (function != NULL) {
            function->loop_depth += 1u;
        }
        check_block(context, scope, function, statement->as.cycle.body);
        if (function != NULL && function->loop_depth > 0u) {
            function->loop_depth -= 1u;
        }
        break;
    case AST_STMT_TRAVERSE: {
        AstStmt *binding = statement->as.traverse.binding;
        Scope loop_scope;
        TypeKind iterable = check_expression(context, scope,
                                             statement->as.traverse.iterable);
        TypeKind element = type_kind_list_element(iterable);
        /* Resolve the iterable before installing the loop-local binding: a
         * same-named outer variable remains visible in that expression. */
        if (iterable == TYPE_LIST_EMPTY) {
            semantic_error_span(context, statement->as.traverse.iterable->span,
                                "cannot traverse an untyped empty list; use a typed binding or function result");
        } else if (iterable != TYPE_ERROR && element == TYPE_UNKNOWN) {
            semantic_error_span(context, statement->as.traverse.iterable != NULL
                                          ? statement->as.traverse.iterable->span
                                          : statement->span,
                                "traverse iterable must be a typed list, got %s",
                                type_kind_name(iterable));
        }
        memset(&loop_scope, 0, sizeof(loop_scope));
        loop_scope.parent = scope;
        if (binding != NULL) {
            binding->as.var_decl.type.kind = element == TYPE_UNKNOWN ? TYPE_ERROR : element;
            symbol = scope_declare(context, &loop_scope, binding->as.var_decl.name,
                                   SYMBOL_VARIABLE, binding->as.var_decl.type.kind,
                                   false, binding, binding->span);
            if (symbol != NULL) symbol->defined = true;
        }
        if (function != NULL) function->loop_depth += 1u;
        check_block(context, &loop_scope, function, statement->as.traverse.body);
        if (function != NULL) function->loop_depth -= 1u;
        scope_free_symbols(&loop_scope);
        break;
    }
    case AST_STMT_ESCAPE:
    case AST_STMT_SKIP:
        if (function == NULL || function->loop_depth == 0u) {
            semantic_error_span(context, statement->span,
                                "%s is only valid inside cycle or traverse",
                                statement->kind == AST_STMT_ESCAPE ? "escape" : "skip");
        }
        break;
    case AST_STMT_SUMMON:
        semantic_error_span(context, statement->span,
                            "unresolved summon; use the source module loader");
        break;
    }
}

static void predeclare_program(SemanticContext *context, AstProgram *program,
                               Scope *global) {
    size_t i;
    for (i = 0u; i < program->item_count; ++i) {
        AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_FUNCTION) {
            AstFunction *function = item->as.function;
            (void)scope_declare(context, global, function->name,
                                SYMBOL_FUNCTION, TYPE_UNKNOWN, false,
                                function, function->span);
        } else if (item->kind == AST_TOP_DECLARATION) {
            AstStmt *statement = item->as.statement;
            (void)scope_declare(context, global, statement->as.var_decl.name,
                                SYMBOL_VARIABLE,
                                statement->as.var_decl.type.kind,
                                statement->as.var_decl.storage == AST_STORAGE_SHIFT,
                                statement, statement->span);
        }
    }
}

static void check_function(SemanticContext *context, Scope *global,
                           AstFunction *function) {
    Scope function_scope;
    FunctionContext function_context;
    size_t i;
    Symbol *symbol;

    memset(&function_scope, 0, sizeof(function_scope));
    function_scope.parent = global;
    memset(&function_context, 0, sizeof(function_context));
    function_context.function = function;
    if (function->return_type.kind == TYPE_UNKNOWN) {
        semantic_error_span(context, function->span,
                            "orbit '%s' needs a return type", function->name);
    }
    for (i = 0u; i < function->parameter_count; ++i) {
        AstParameter *parameter = &function->parameters[i];
        if (parameter->type.kind == TYPE_UNKNOWN) {
            semantic_error_span(context, parameter->span,
                                "parameter '%s' needs a type", parameter->name);
        }
        if (parameter->type.kind == TYPE_SILENT) {
            semantic_error_span(context, parameter->span, "silent is not a parameter value type");
        }
        symbol = scope_declare(context, &function_scope, parameter->name,
                               SYMBOL_PARAMETER, parameter->type.kind, false,
                               parameter, parameter->span);
        if (symbol != NULL) {
            symbol->defined = true;
        }
    }
    check_block(context, &function_scope, &function_context, function->body);
    if (function->return_type.kind != TYPE_SILENT &&
        function->return_type.kind != TYPE_UNKNOWN &&
        !block_always_yields(function->body)) {
        semantic_error_span(context, function->span,
                            "orbit '%s' does not yield its declared return type on every path",
                            function->name);
    }
    scope_free_symbols(&function_scope);
}

bool semantic_check(AstProgram *program, Diagnostics *diagnostics) {
    SemanticContext context;
    Scope global;
    size_t i;

    if (program == NULL) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    context.diagnostics = diagnostics;
    context.ok = !program->had_parse_error;
    memset(&global, 0, sizeof(global));
    context.global = &global;
    predeclare_program(&context, program, &global);
    /* Resolve global initializers first so function bodies can use the
     * resulting signatures/types regardless of source ordering. */
    for (i = 0u; i < program->item_count; ++i) {
        AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_DECLARATION) {
            AstStmt *statement = item->as.statement;
            Symbol *symbol = scope_find_local(&global,
                                              statement->as.var_decl.name);
            if (symbol != NULL && symbol->kind == SYMBOL_VARIABLE &&
                symbol->as.declaration == statement) {
                check_declaration(&context, &global, statement, symbol);
            }
        } else if (item->kind == AST_TOP_EXPRESSION) {
            FunctionContext top_level;
            memset(&top_level, 0, sizeof(top_level));
            check_statement(&context, &global, &top_level, item->as.statement);
        }
    }
    for (i = 0u; i < program->item_count; ++i) {
        AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_FUNCTION) {
            Symbol *symbol = scope_find_local(&global, item->as.function->name);
            if (symbol != NULL && symbol->kind == SYMBOL_FUNCTION &&
                symbol->as.function == item->as.function) {
                check_function(&context, &global, item->as.function);
            }
        }
    }
    scope_free_symbols(&global);
    return context.ok;
}
