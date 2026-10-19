#ifndef APHELION_AST_H
#define APHELION_AST_H

/*
 * The syntax tree is deliberately independent of the byte-code compiler.  It
 * contains only source level information and the small amount of information
 * (inferred_type/resolved_symbol) which a later semantic pass needs to attach
 * to an expression.  Strings owned by the tree are allocated with malloc and
 * are released by ast_program_free().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aphelion/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SourceSpan {
    SourceLocation start;
    SourceLocation end; /* points just after the last source byte */
    size_t start_offset; /* byte offsets are useful to source tooling */
    size_t end_offset;
} SourceSpan;

/* Scalar types and invariant lists of supported scalar elements. */
typedef enum TypeKind {
    TYPE_UNKNOWN = 0,
    TYPE_ERROR,
    TYPE_WHOLE,
    TYPE_DECIMAL,
    TYPE_SIGNAL,
    TYPE_TEXT,
    TYPE_GLYPH,
    TYPE_SILENT,
    TYPE_HOLLOW,
    TYPE_LIST_WHOLE,
    TYPE_LIST_DECIMAL,
    TYPE_LIST_SIGNAL,
    TYPE_LIST_TEXT,
    TYPE_LIST_GLYPH,
    TYPE_LIST_EMPTY
} TypeKind;

/* Friendly aliases used by some clients of the public API. */
#define AST_TYPE_UNKNOWN TYPE_UNKNOWN
#define AST_TYPE_ERROR TYPE_ERROR
#define AST_TYPE_WHOLE TYPE_WHOLE
#define AST_TYPE_DECIMAL TYPE_DECIMAL
#define AST_TYPE_SIGNAL TYPE_SIGNAL
#define AST_TYPE_TEXT TYPE_TEXT
#define AST_TYPE_GLYPH TYPE_GLYPH
#define AST_TYPE_SILENT TYPE_SILENT
#define AST_TYPE_HOLLOW TYPE_HOLLOW
#define AST_TYPE_LIST_WHOLE TYPE_LIST_WHOLE
#define AST_TYPE_LIST_DECIMAL TYPE_LIST_DECIMAL
#define AST_TYPE_LIST_SIGNAL TYPE_LIST_SIGNAL
#define AST_TYPE_LIST_TEXT TYPE_LIST_TEXT
#define AST_TYPE_LIST_GLYPH TYPE_LIST_GLYPH
#define AST_TYPE_LIST_EMPTY TYPE_LIST_EMPTY

typedef struct AstType {
    TypeKind kind;
    char *name;           /* optional spelling, owned by the node */
    bool explicitly_typed;
} AstType;

typedef enum AstLiteralKind {
    AST_LITERAL_WHOLE = 0,
    AST_LITERAL_DECIMAL,
    AST_LITERAL_SIGNAL,
    AST_LITERAL_TEXT,
    AST_LITERAL_GLYPH,
    AST_LITERAL_HOLLOW
} AstLiteralKind;

typedef enum AstExprKind {
    AST_EXPR_LITERAL = 0,
    AST_EXPR_NAME,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_CALL,
    AST_EXPR_ASSIGN,
    AST_EXPR_GROUP,
    AST_EXPR_LIST,
    AST_EXPR_INDEX
} AstExprKind;

typedef enum AstUnaryOp {
    AST_UNARY_NOT = 0,
    AST_UNARY_NEGATE,
    AST_UNARY_PLUS
} AstUnaryOp;

typedef enum AstBinaryOp {
    AST_BINARY_ADD = 0,
    AST_BINARY_SUBTRACT,
    AST_BINARY_MULTIPLY,
    AST_BINARY_DIVIDE,
    AST_BINARY_REMAINDER,
    AST_BINARY_EQUAL,
    AST_BINARY_NOT_EQUAL,
    AST_BINARY_LESS,
    AST_BINARY_LESS_EQUAL,
    AST_BINARY_GREATER,
    AST_BINARY_GREATER_EQUAL,
    AST_BINARY_AND,
    AST_BINARY_OR
} AstBinaryOp;

typedef enum AstAssignOp {
    AST_ASSIGN_SET = 0,
    AST_ASSIGN_ADD,
    AST_ASSIGN_SUBTRACT,
    AST_ASSIGN_MULTIPLY,
    AST_ASSIGN_DIVIDE,
    AST_ASSIGN_REMAINDER
} AstAssignOp;

typedef struct AstExpr AstExpr;
typedef struct AstStmt AstStmt;
typedef struct AstBlock AstBlock;
typedef struct AstFunction AstFunction;

struct AstExpr {
    AstExprKind kind;
    SourceSpan span;
    unsigned depth; /* parser-maintained tree depth, bounded before recursion */
    TypeKind inferred_type;
    void *resolved_symbol; /* borrowed declaration/function node, if resolved */

    union {
        struct {
            AstLiteralKind kind;
            int64_t whole;
            double decimal;
            bool signal;
            uint32_t glyph;
            char *text;
            size_t text_length; /* includes embedded NUL bytes, excludes terminator */
        } literal;
        struct {
            char *name;
        } name;
        struct {
            AstUnaryOp op;
            AstExpr *operand;
        } unary;
        struct {
            AstBinaryOp op;
            AstExpr *left;
            AstExpr *right;
        } binary;
        struct {
            AstExpr *callee;
            AstExpr **arguments;
            size_t argument_count;
            size_t argument_capacity;
        } call;
        struct {
            AstAssignOp op;
            AstExpr *target;
            AstExpr *value;
        } assignment;
        struct {
            AstExpr *inner;
        } group;
        struct {
            AstExpr **elements;
            size_t element_count;
            size_t element_capacity;
        } list;
        struct {
            AstExpr *target;
            AstExpr *index;
        } index;
    } as;
};

typedef enum AstStorageKind {
    AST_STORAGE_FORGE = 0,
    AST_STORAGE_SHIFT,
    AST_STORAGE_ANCHOR
} AstStorageKind;

typedef enum AstStmtKind {
    AST_STMT_VAR_DECL = 0,
    AST_STMT_EXPRESSION,
    AST_STMT_BLOCK,
    AST_STMT_YIELD,
    AST_STMT_WHEN,
    AST_STMT_CYCLE,
    AST_STMT_ESCAPE,
    AST_STMT_SKIP,
    AST_STMT_TRAVERSE,
    AST_STMT_SUMMON
} AstStmtKind;

struct AstBlock {
    SourceSpan span;
    AstStmt **statements;
    size_t statement_count;
    size_t statement_capacity;
};

struct AstStmt {
    AstStmtKind kind;
    SourceSpan span;
    union {
        struct {
            AstStorageKind storage;
            char *name;
            AstType type;
            AstExpr *initializer;
        } var_decl;
        struct {
            AstExpr *expression;
        } expression;
        struct {
            AstBlock *block;
        } block;
        struct {
            AstExpr *expression; /* NULL means `yield` (silent return) */
        } yield;
        struct {
            AstExpr *condition;
            AstBlock *then_branch;
            AstBlock *else_branch; /* NULL when `otherwise` is absent */
        } when_stmt;
        struct {
            AstExpr *condition;
            AstBlock *body;
        } cycle;
        struct {
            /* Owned internal forge declaration; its node is symbol identity.
             * No initializer: the iterable supplies each iteration's value. */
            AstStmt *binding;
            AstExpr *iterable;
            AstBlock *body;
        } traverse;
        struct {
            char *path; /* owned decoded import name */
            size_t path_length;
        } summon;
    } as;
};

typedef struct AstParameter {
    SourceSpan span;
    char *name;
    AstType type;
} AstParameter;

struct AstFunction {
    SourceSpan span;
    char *name;
    AstParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    AstType return_type;
    AstBlock *body;
};

typedef enum AstTopLevelKind {
    AST_TOP_FUNCTION = 0,
    AST_TOP_DECLARATION,
    AST_TOP_EXPRESSION
} AstTopLevelKind;

typedef struct AstTopLevel {
    AstTopLevelKind kind;
    SourceSpan span;
    union {
        AstFunction *function;
        AstStmt *statement;
    } as;
} AstTopLevel;

typedef struct AstProgram {
    char *filename;
    AstTopLevel *items;
    size_t item_count;
    size_t item_capacity;
    bool had_parse_error;
    char **source_files; /* owned canonical filenames borrowed by linked spans */
    size_t source_file_count;
    size_t source_file_capacity;
} AstProgram;

/* Basic constructors used by parser clients and by parser.c. */
AstProgram *ast_program_new(const char *filename);
AstFunction *ast_function_new(SourceSpan span, const char *name);
AstBlock *ast_block_new(SourceSpan span);
AstStmt *ast_stmt_new(AstStmtKind kind, SourceSpan span);
AstExpr *ast_expr_new(AstExprKind kind, SourceSpan span);

bool ast_program_add_item(AstProgram *program, AstTopLevel item);
bool ast_block_add_statement(AstBlock *block, AstStmt *statement);
bool ast_function_add_parameter(AstFunction *function, AstParameter parameter);
bool ast_call_add_argument(AstExpr *call, AstExpr *argument);
bool ast_list_add_element(AstExpr *list, AstExpr *element);

/* Frees a complete tree, including all strings and child nodes. */
void ast_program_free(AstProgram *program);
/* Also used to release partial trees when parser allocations fail. */
void ast_expr_free(AstExpr *expression);
void ast_stmt_free(AstStmt *statement);
void ast_function_free(AstFunction *function);

const char *type_kind_name(TypeKind type);
bool type_kind_is_numeric(TypeKind type);
bool type_kind_is_value(TypeKind type);
bool type_kind_is_list(TypeKind type);
/* Unsupported element/non-list types return TYPE_UNKNOWN. */
TypeKind type_kind_list_of(TypeKind element);
TypeKind type_kind_list_element(TypeKind list);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_AST_H */
