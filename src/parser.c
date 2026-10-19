#include "aphelion/parser.h"

#include "aphelion/lexer.h"
#include "aphelion/token.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This parser consumes the public Lexer/Token API.  In particular it does not
 * duplicate lexical rules (comments, escapes, or keyword spelling) here.  A
 * one-token lookahead is enough for the grammar, and the expression parser is
 * a conventional precedence-climbing recursive descent parser.
 */

typedef struct Parser {
    Lexer lexer;
    const char *filename;
    const char *source;
    AstProgram *program;
    Diagnostics *diagnostics;
    Token current;
    Token previous;
    bool had_error;
    bool fatal;
    unsigned recursion_depth;
} Parser;

#define PARSER_DEPTH_LIMIT 128u

static char *copy_bytes(const char *text, size_t length) {
    char *copy;
    if (text == NULL) {
        return NULL;
    }
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

static char *copy_token_text(const Token *token) {
    if (token == NULL) {
        return NULL;
    }
    return copy_bytes(token->lexeme, token->lexeme_length);
}

static int parser_hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

/* String lexemes are accepted both with and without delimiters.  The lexer
 * normally returns the source span, so decoding here keeps the AST stable if
 * a client supplies a lexer that returns an interior-only span. */
static char *copy_string_token(const Token *token, size_t *decoded_length) {
    const char *text;
    size_t length;
    size_t first = 0u;
    size_t last;
    size_t i;
    size_t out_length = 0u;
    char *out;

    if (token == NULL || token->lexeme == NULL) {
        return NULL;
    }
    text = token->lexeme;
    length = token->lexeme_length;
    last = length;
    if (length >= 2u && ((text[0] == '"' && text[length - 1u] == '"') ||
                         (text[0] == '\'' && text[length - 1u] == '\''))) {
        first = 1u;
        last = length - 1u;
    }
    for (i = first; i < last; ++i) {
        if (text[i] == '\\' && i + 1u < last) {
            if (text[i + 1u] == 'x' && i + 3u < last &&
                parser_hex_value(text[i + 2u]) >= 0 &&
                parser_hex_value(text[i + 3u]) >= 0) {
                i += 3u;
            } else {
                ++i;
            }
        }
        ++out_length;
    }
    out = (char *)malloc(out_length + 1u);
    if (out == NULL) {
        return NULL;
    }
    out_length = 0u;
    for (i = first; i < last; ++i) {
        char value = text[i];
        if (value == '\\' && i + 1u < last) {
            char escaped = text[++i];
            switch (escaped) {
            case 'a':
                value = '\a';
                break;
            case 'b':
                value = '\b';
                break;
            case 'f':
                value = '\f';
                break;
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case 'v':
                value = '\v';
                break;
            case '0':
                value = '\0';
                break;
            case '\\':
                value = '\\';
                break;
            case '\'':
                value = '\'';
                break;
            case '"':
                value = '"';
                break;
            case 'x':
                if (i + 2u < last && parser_hex_value(text[i + 1u]) >= 0 &&
                    parser_hex_value(text[i + 2u]) >= 0) {
                    value = (char)(parser_hex_value(text[i + 1u]) * 16 +
                                   parser_hex_value(text[i + 2u]));
                    i += 2u;
                } else {
                    value = escaped;
                }
                break;
            default:
                value = escaped;
                break;
            }
        }
        out[out_length++] = value;
    }
    out[out_length] = '\0';
    if (decoded_length != NULL) *decoded_length = out_length;
    return out;
}

static SourceSpan empty_span(const char *filename) {
    SourceSpan span;
    memset(&span, 0, sizeof(span));
    span.start.filename = filename;
    span.end.filename = filename;
    span.start.line = 1u;
    span.start.column = 1u;
    span.end.line = 1u;
    span.end.column = 1u;
    return span;
}

static SourceLocation token_end_location(const Token *token) {
    SourceLocation end;
    size_t i;
    end = token->location;
    for (i = 0u; i < token->lexeme_length; ++i) {
        end.offset += 1u;
        if (token->lexeme[i] == '\r') {
            end.line += 1u;
            end.column = 1u;
            if (i + 1u < token->lexeme_length && token->lexeme[i + 1u] == '\n') {
                ++i;
                end.offset += 1u;
            }
        } else if (token->lexeme[i] == '\n') {
            end.line += 1u;
            end.column = 1u;
        } else {
            end.column += 1u;
        }
    }
    return end;
}

static SourceSpan token_span(const Parser *parser, const Token *token) {
    SourceSpan span;
    memset(&span, 0, sizeof(span));
    if (token == NULL) {
        return empty_span(parser != NULL ? parser->filename : NULL);
    }
    span.start = token->location;
    span.end = token_end_location(token);
    span.start_offset = token->location.offset;
    span.end_offset = span.start_offset + token->lexeme_length;
    return span;
}

static SourceSpan span_join(SourceSpan first, SourceSpan last) {
    SourceSpan result = first;
    result.end = last.end;
    result.end_offset = last.end_offset;
    return result;
}

static void parser_report(Parser *parser, SourceLocation location,
                          const char *format, ...) {
    char message[512];
    va_list arguments;

    parser->had_error = true;
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (parser->diagnostics != NULL) {
        (void)diagnostics_add(parser->diagnostics, location, message);
    }
    if (strncmp(message, "out of memory", 13u) == 0) {
        parser->fatal = true;
        parser->current.kind = TOKEN_EOF;
    }
}

static void parser_report_current(Parser *parser, const char *format, ...) {
    char message[512];
    va_list arguments;

    parser->had_error = true;
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (parser->diagnostics != NULL) {
        (void)diagnostics_add(parser->diagnostics, parser->current.location,
                              message);
    }
}

static bool at(Parser *parser, TokenKind kind) {
    return parser->current.kind == kind;
}

static void advance_token(Parser *parser) {
    if (parser->fatal) { parser->current.kind = TOKEN_EOF; return; }
    parser->previous = parser->current;
    parser->current = lexer_next(&parser->lexer);
}

static bool match(Parser *parser, TokenKind kind) {
    if (!at(parser, kind)) {
        return false;
    }
    advance_token(parser);
    return true;
}

static bool expect(Parser *parser, TokenKind kind, const char *description) {
    if (match(parser, kind)) {
        return true;
    }
    parser_report_current(parser, "expected %s, found %s", description,
                          token_kind_name(parser->current.kind));
    return false;
}

static void consume_separators(Parser *parser) {
    while (at(parser, TOKEN_NEWLINE) || at(parser, TOKEN_SEMICOLON)) {
        advance_token(parser);
    }
}

/* A newline after an operator or opening delimiter continues the expression;
 * a newline after a complete expression remains a statement separator. */
static void consume_expression_newlines(Parser *parser) {
    while (at(parser, TOKEN_NEWLINE)) {
        advance_token(parser);
    }
}

static void synchronize(Parser *parser) {
    while (!at(parser, TOKEN_EOF) && !at(parser, TOKEN_NEWLINE) &&
           !at(parser, TOKEN_SEMICOLON) && !at(parser, TOKEN_RIGHT_BRACE)) {
        advance_token(parser);
    }
    consume_separators(parser);
}

/* Recovery at file scope must make progress even when a stray closing brace
 * is the first synchronization token.  Blocks deliberately leave that brace
 * for their own closing-brace logic, so this helper is only used by the
 * program loop. */
static void synchronize_top_level(Parser *parser) {
    TokenKind before = parser->current.kind;
    synchronize(parser);
    if (at(parser, TOKEN_RIGHT_BRACE)) {
        advance_token(parser);
    } else if (parser->current.kind == before && !at(parser, TOKEN_EOF)) {
        advance_token(parser);
    }
}

static bool is_type_token(TokenKind kind) {
    return kind == TOKEN_KW_WHOLE || kind == TOKEN_KW_DECIMAL ||
           kind == TOKEN_KW_SIGNAL || kind == TOKEN_KW_TEXT ||
           kind == TOKEN_KW_GLYPH || kind == TOKEN_KW_SILENT ||
           kind == TOKEN_LEFT_BRACKET;
}

static AstType parse_type(Parser *parser) {
    AstType type;
    Token token = parser->current;
    type.kind = TYPE_UNKNOWN;
    type.name = NULL;
    type.explicitly_typed = false;
    if (token.kind == TOKEN_LEFT_BRACKET) {
        /* Only scalar element annotations are supported; do not recurse on
         * brackets, including malformed/nested annotation input. */
        advance_token(parser);
        consume_expression_newlines(parser);
        if (!is_type_token(parser->current.kind) ||
            at(parser, TOKEN_LEFT_BRACKET) || at(parser, TOKEN_KW_SILENT)) {
            parser_report_current(parser, "list element type must be whole, decimal, signal, text, or glyph");
            type.kind = TYPE_ERROR;
            return type;
        }
        type = parse_type(parser);
        type.kind = type_kind_list_of(type.kind);
        free(type.name);
        type.name = NULL;
        consume_expression_newlines(parser);
        (void)expect(parser, TOKEN_RIGHT_BRACKET, "']' after list element type");
        return type;
    }
    if (!is_type_token(token.kind)) {
        parser_report_current(parser, "expected a primitive type, found %s",
                              token_kind_name(token.kind));
        return type;
    }
    type.explicitly_typed = true;
    switch (token.kind) {
    case TOKEN_KW_WHOLE:
        type.kind = TYPE_WHOLE;
        break;
    case TOKEN_KW_DECIMAL:
        type.kind = TYPE_DECIMAL;
        break;
    case TOKEN_KW_SIGNAL:
        type.kind = TYPE_SIGNAL;
        break;
    case TOKEN_KW_TEXT:
        type.kind = TYPE_TEXT;
        break;
    case TOKEN_KW_GLYPH:
        type.kind = TYPE_GLYPH;
        break;
    case TOKEN_KW_SILENT:
        type.kind = TYPE_SILENT;
        break;
    default:
        break;
    }
    type.name = copy_token_text(&token);
    if (type.name == NULL) {
        parser_report(parser, token.location, "out of memory copying type name");
    }
    advance_token(parser);
    return type;
}

static AstExpr *parse_expression(Parser *parser);
static AstStmt *parse_statement(Parser *parser);
static AstBlock *parse_block(Parser *parser);

static bool enter_recursion(Parser *parser) {
    if (parser->fatal) return false;
    if (parser->recursion_depth >= PARSER_DEPTH_LIMIT) {
        parser_report_current(parser, "syntax nesting limit exceeded (128)");
        parser->fatal = true;
        parser->current.kind = TOKEN_EOF;
        return false;
    }
    ++parser->recursion_depth;
    return true;
}

static AstExpr *check_tree_depth(Parser *parser, AstExpr *expression) {
    unsigned depth = 0u;
    size_t i;
    if (expression == NULL) return NULL;
    switch (expression->kind) {
    case AST_EXPR_UNARY: depth = expression->as.unary.operand->depth; break;
    case AST_EXPR_GROUP: depth = expression->as.group.inner->depth; break;
    case AST_EXPR_BINARY:
        depth = expression->as.binary.left->depth;
        if (expression->as.binary.right->depth > depth)
            depth = expression->as.binary.right->depth;
        break;
    case AST_EXPR_ASSIGN:
        depth = expression->as.assignment.target->depth;
        if (expression->as.assignment.value->depth > depth)
            depth = expression->as.assignment.value->depth;
        break;
    case AST_EXPR_CALL:
        depth = expression->as.call.callee->depth;
        for (i = 0u; i < expression->as.call.argument_count; ++i) {
            if (expression->as.call.arguments[i]->depth > depth)
                depth = expression->as.call.arguments[i]->depth;
        }
        break;
    case AST_EXPR_LIST:
        for (i = 0u; i < expression->as.list.element_count; ++i) {
            if (expression->as.list.elements[i]->depth > depth)
                depth = expression->as.list.elements[i]->depth;
        }
        break;
    case AST_EXPR_INDEX:
        depth = expression->as.index.target->depth;
        if (expression->as.index.index != NULL &&
            expression->as.index.index->depth > depth)
            depth = expression->as.index.index->depth;
        break;
    default: break;
    }
    expression->depth = depth + 1u;
    if (expression->depth > PARSER_DEPTH_LIMIT) {
        parser_report(parser, expression->span.start, "expression depth limit exceeded (128)");
        parser->fatal = true;
        parser->current.kind = TOKEN_EOF;
        ast_expr_free(expression);
        return NULL;
    }
    return expression;
}

static bool statement_has_brace_boundary(const AstStmt *statement) {
    if (statement == NULL) {
        return false;
    }
    return statement->kind == AST_STMT_BLOCK ||
           statement->kind == AST_STMT_WHEN ||
           statement->kind == AST_STMT_CYCLE ||
           statement->kind == AST_STMT_TRAVERSE;
}

static AstExpr *new_literal(Parser *parser, AstLiteralKind kind,
                            SourceSpan span) {
    AstExpr *expression = ast_expr_new(AST_EXPR_LITERAL, span);
    if (expression == NULL) {
        parser_report(parser, span.start, "out of memory while creating literal");
        return NULL;
    }
    expression->as.literal.kind = kind;
    return expression;
}

static AstExpr *parse_primary(Parser *parser) {
    Token token = parser->current;
    SourceSpan span = token_span(parser, &token);
    AstExpr *expression = NULL;

    switch (token.kind) {
    case TOKEN_WHOLE_LITERAL:
        if (token.whole_min_magnitude) {
            parser_report(parser, token.location, "positive whole literal is out of range");
        }
        expression = new_literal(parser, AST_LITERAL_WHOLE, span);
        if (expression != NULL) {
            expression->as.literal.whole = token.whole_value;
        }
        advance_token(parser);
        return expression;
    case TOKEN_DECIMAL_LITERAL:
        expression = new_literal(parser, AST_LITERAL_DECIMAL, span);
        if (expression != NULL) {
            expression->as.literal.decimal = token.decimal_value;
        }
        advance_token(parser);
        return expression;
    case TOKEN_STRING_LITERAL:
        expression = new_literal(parser, AST_LITERAL_TEXT, span);
        if (expression != NULL) {
            expression->as.literal.text = copy_string_token(&token,
                                                 &expression->as.literal.text_length);
            if (expression->as.literal.text == NULL) {
                parser_report(parser, token.location,
                              "out of memory while copying text literal");
            }
        }
        advance_token(parser);
        return expression;
    case TOKEN_GLYPH_LITERAL:
        expression = new_literal(parser, AST_LITERAL_GLYPH, span);
        if (expression != NULL) {
            expression->as.literal.glyph = (uint32_t)token.glyph_value;
        }
        advance_token(parser);
        return expression;
    case TOKEN_KW_BRIGHT:
    case TOKEN_KW_DIM:
        expression = new_literal(parser, AST_LITERAL_SIGNAL, span);
        if (expression != NULL) {
            expression->as.literal.signal = token.kind == TOKEN_KW_BRIGHT;
        }
        advance_token(parser);
        return expression;
    case TOKEN_KW_HOLLOW:
        expression = new_literal(parser, AST_LITERAL_HOLLOW, span);
        advance_token(parser);
        return expression;
    case TOKEN_IDENTIFIER:
    case TOKEN_KW_REVEAL:
    case TOKEN_KW_RECEIVE:
        expression = ast_expr_new(AST_EXPR_NAME, span);
        if (expression == NULL) {
            parser_report(parser, token.location,
                          "out of memory while creating identifier");
        } else {
            expression->as.name.name = copy_token_text(&token);
            if (expression->as.name.name == NULL) {
                parser_report(parser, token.location,
                              "out of memory while copying identifier");
            }
        }
        advance_token(parser);
        return expression;
    case TOKEN_LEFT_BRACKET: {
        expression = ast_expr_new(AST_EXPR_LIST, span);
        if (expression == NULL) {
            parser_report(parser, token.location, "out of memory while creating list");
            return NULL;
        }
        advance_token(parser);
        consume_expression_newlines(parser);
        while (!at(parser, TOKEN_RIGHT_BRACKET) && !at(parser, TOKEN_EOF)) {
            AstExpr *element = parse_expression(parser);
            if (element == NULL) break;
            if (!ast_list_add_element(expression, element)) {
                parser_report(parser, element->span.start,
                              "out of memory while adding list element");
                ast_expr_free(element);
                break;
            }
            consume_expression_newlines(parser);
            if (!match(parser, TOKEN_COMMA)) break;
            consume_expression_newlines(parser);
        }
        (void)expect(parser, TOKEN_RIGHT_BRACKET, "']' after list elements");
        expression->span = span_join(span, token_span(parser, &parser->previous));
        return check_tree_depth(parser, expression);
    }
    case TOKEN_LEFT_PAREN: {
        SourceSpan start = span;
        advance_token(parser);
        consume_expression_newlines(parser);
        expression = parse_expression(parser);
        consume_expression_newlines(parser);
        if (!expect(parser, TOKEN_RIGHT_PAREN, "')'")) {
            return expression;
        }
        if (expression != NULL) {
            AstExpr *group = ast_expr_new(AST_EXPR_GROUP,
                                          span_join(start,
                                                    token_span(parser,
                                                               &parser->previous)));
            if (group == NULL) {
                parser_report(parser, start.start,
                              "out of memory while creating grouping");
                return expression;
            }
            group->as.group.inner = expression;
            return check_tree_depth(parser, group);
        }
        return NULL;
    }
    case TOKEN_ERROR:
        /* The lexer has already recorded the useful spelling-specific
         * diagnostic.  Consume the token without adding a cascade error. */
        parser->had_error = true;
        advance_token(parser);
        return NULL;
    default:
        parser_report_current(parser, "expected an expression, found %s",
                              token_kind_name(token.kind));
        return NULL;
    }
}

static AstExpr *parse_postfix(Parser *parser) {
    AstExpr *expression = parse_primary(parser);
    while (expression != NULL &&
           (at(parser, TOKEN_LEFT_PAREN) || at(parser, TOKEN_LEFT_BRACKET))) {
        SourceSpan start = expression->span;
        AstExpr *call;
        if (at(parser, TOKEN_LEFT_BRACKET)) {
            AstExpr *index = ast_expr_new(AST_EXPR_INDEX, start);
            if (index == NULL) {
                parser_report(parser, start.start, "out of memory while creating index");
                return expression;
            }
            index->as.index.target = expression;
            advance_token(parser);
            index->as.index.index = parse_expression(parser);
            consume_expression_newlines(parser);
            (void)expect(parser, TOKEN_RIGHT_BRACKET, "']' after index");
            index->span = span_join(start, token_span(parser, &parser->previous));
            expression = check_tree_depth(parser, index);
            continue;
        }
        advance_token(parser);
        call = ast_expr_new(AST_EXPR_CALL, start);
        if (call == NULL) {
            parser_report(parser, start.start, "out of memory while creating call");
            return expression;
        }
        call->as.call.callee = expression;
        consume_expression_newlines(parser);
        if (!at(parser, TOKEN_RIGHT_PAREN)) {
            for (;;) {
                AstExpr *argument = parse_expression(parser);
                if (argument == NULL) {
                    break;
                }
                if (!ast_call_add_argument(call, argument)) {
                    parser_report(parser, argument->span.start,
                                  "out of memory while adding call argument");
                    ast_expr_free(argument);
                    break;
                }
                consume_expression_newlines(parser);
                if (!match(parser, TOKEN_COMMA) || at(parser, TOKEN_RIGHT_PAREN)) {
                    break;
                }
                consume_expression_newlines(parser);
            }
        }
        consume_expression_newlines(parser);
        if (!expect(parser, TOKEN_RIGHT_PAREN, "')'")) {
            /* Keep the partial call so the caller can recover and free it. */
            expression = call;
            break;
        }
        call->span = span_join(start, token_span(parser, &parser->previous));
        expression = check_tree_depth(parser, call);
    }
    return expression;
}

static AstExpr *parse_unary_impl(Parser *parser);
static AstExpr *parse_unary(Parser *parser) {
    AstExpr *result;
    if (!enter_recursion(parser)) return NULL;
    result = parse_unary_impl(parser);
    --parser->recursion_depth;
    return result;
}

static AstExpr *parse_unary_impl(Parser *parser) {
    Token token = parser->current;
    if (token.kind == TOKEN_BANG || token.kind == TOKEN_KW_NOT ||
        token.kind == TOKEN_MINUS || token.kind == TOKEN_PLUS) {
        AstUnaryOp op;
        SourceSpan start = token_span(parser, &token);
        AstExpr *operand;
        AstExpr *expression;
        advance_token(parser);
        consume_expression_newlines(parser);
        if (token.kind == TOKEN_MINUS &&
            at(parser, TOKEN_WHOLE_LITERAL) && parser->current.whole_min_magnitude) {
            expression = new_literal(parser, AST_LITERAL_WHOLE,
                span_join(start, token_span(parser, &parser->current)));
            if (expression != NULL) expression->as.literal.whole = INT64_MIN;
            advance_token(parser);
            return expression;
        }
        if (token.kind == TOKEN_BANG || token.kind == TOKEN_KW_NOT) {
            op = AST_UNARY_NOT;
        } else if (token.kind == TOKEN_MINUS) {
            op = AST_UNARY_NEGATE;
        } else {
            op = AST_UNARY_PLUS;
        }
        operand = parse_unary(parser);
        if (operand == NULL) {
            return NULL;
        }
        expression = ast_expr_new(AST_EXPR_UNARY,
                                  span_join(start, operand->span));
        if (expression == NULL) {
            parser_report(parser, start.start,
                          "out of memory while creating unary expression");
            return operand;
        }
        expression->as.unary.op = op;
        expression->as.unary.operand = operand;
        return check_tree_depth(parser, expression);
    }
    return parse_postfix(parser);
}

static int binary_precedence(TokenKind kind) {
    switch (kind) {
    case TOKEN_OR_OR:
    case TOKEN_KW_OR:
        return 1;
    case TOKEN_AND_AND:
    case TOKEN_KW_AND:
        return 2;
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_NOT_EQUAL:
        return 3;
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return 4;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return 5;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
        return 6;
    default:
        return 0;
    }
}

static AstBinaryOp binary_operator(TokenKind kind) {
    switch (kind) {
    case TOKEN_PLUS:
        return AST_BINARY_ADD;
    case TOKEN_MINUS:
        return AST_BINARY_SUBTRACT;
    case TOKEN_STAR:
        return AST_BINARY_MULTIPLY;
    case TOKEN_SLASH:
        return AST_BINARY_DIVIDE;
    case TOKEN_PERCENT:
        return AST_BINARY_REMAINDER;
    case TOKEN_EQUAL_EQUAL:
        return AST_BINARY_EQUAL;
    case TOKEN_NOT_EQUAL:
        return AST_BINARY_NOT_EQUAL;
    case TOKEN_LESS:
        return AST_BINARY_LESS;
    case TOKEN_LESS_EQUAL:
        return AST_BINARY_LESS_EQUAL;
    case TOKEN_GREATER:
        return AST_BINARY_GREATER;
    case TOKEN_GREATER_EQUAL:
        return AST_BINARY_GREATER_EQUAL;
    case TOKEN_AND_AND:
    case TOKEN_KW_AND:
        return AST_BINARY_AND;
    case TOKEN_OR_OR:
    case TOKEN_KW_OR:
    default:
        return AST_BINARY_OR;
    }
}

static AstExpr *parse_binary(Parser *parser, int minimum_precedence) {
    AstExpr *left = parse_unary(parser);
    while (left != NULL) {
        int precedence = binary_precedence(parser->current.kind);
        TokenKind operator_kind;
        AstExpr *right;
        AstExpr *expression;
        if (precedence < minimum_precedence || precedence == 0) {
            break;
        }
        operator_kind = parser->current.kind;
        advance_token(parser);
        consume_expression_newlines(parser);
        right = parse_binary(parser, precedence + 1);
        if (right == NULL) {
            break;
        }
        expression = ast_expr_new(AST_EXPR_BINARY,
                                  span_join(left->span, right->span));
        if (expression == NULL) {
            parser_report(parser, left->span.start,
                          "out of memory while creating binary expression");
            ast_expr_free(right);
            return left;
        }
        expression->as.binary.op = binary_operator(operator_kind);
        expression->as.binary.left = left;
        expression->as.binary.right = right;
        left = check_tree_depth(parser, expression);
    }
    return left;
}

static bool is_assignment_token(TokenKind kind) {
    return kind == TOKEN_EQUAL || kind == TOKEN_PLUS_EQUAL ||
           kind == TOKEN_MINUS_EQUAL || kind == TOKEN_STAR_EQUAL ||
           kind == TOKEN_SLASH_EQUAL;
}

static AstAssignOp assignment_operator(TokenKind kind) {
    switch (kind) {
    case TOKEN_PLUS_EQUAL:
        return AST_ASSIGN_ADD;
    case TOKEN_MINUS_EQUAL:
        return AST_ASSIGN_SUBTRACT;
    case TOKEN_STAR_EQUAL:
        return AST_ASSIGN_MULTIPLY;
    case TOKEN_SLASH_EQUAL:
        return AST_ASSIGN_DIVIDE;
    case TOKEN_EQUAL:
    default:
        return AST_ASSIGN_SET;
    }
}

static AstExpr *parse_assignment_impl(Parser *parser);
static AstExpr *parse_assignment(Parser *parser) {
    AstExpr *result;
    if (!enter_recursion(parser)) return NULL;
    result = parse_assignment_impl(parser);
    --parser->recursion_depth;
    return result;
}

static AstExpr *parse_assignment_impl(Parser *parser) {
    AstExpr *left = parse_binary(parser, 1);
    if (left != NULL && is_assignment_token(parser->current.kind)) {
        TokenKind kind = parser->current.kind;
        AstExpr *right;
        AstExpr *assignment;
        advance_token(parser);
        consume_expression_newlines(parser);
        right = parse_assignment(parser);
        if (right == NULL) {
            return left;
        }
        assignment = ast_expr_new(AST_EXPR_ASSIGN,
                                  span_join(left->span, right->span));
        if (assignment == NULL) {
            parser_report(parser, left->span.start,
                          "out of memory while creating assignment");
            ast_expr_free(right);
            return left;
        }
        assignment->as.assignment.op = assignment_operator(kind);
        assignment->as.assignment.target = left;
        assignment->as.assignment.value = right;
        return check_tree_depth(parser, assignment);
    }
    return left;
}

static AstExpr *parse_expression(Parser *parser) {
    consume_expression_newlines(parser);
    return parse_assignment(parser);
}

static AstStmt *parse_declaration(Parser *parser) {
    Token keyword = parser->current;
    AstStorageKind storage;
    SourceSpan span = token_span(parser, &keyword);
    AstStmt *statement;
    AstType explicit_type;

    if (keyword.kind == TOKEN_KW_FORGE) {
        storage = AST_STORAGE_FORGE;
    } else if (keyword.kind == TOKEN_KW_SHIFT) {
        storage = AST_STORAGE_SHIFT;
    } else {
        storage = AST_STORAGE_ANCHOR;
    }
    advance_token(parser);
    statement = ast_stmt_new(AST_STMT_VAR_DECL, span);
    if (statement == NULL) {
        parser_report(parser, keyword.location,
                      "out of memory while creating declaration");
        return NULL;
    }
    statement->as.var_decl.storage = storage;
    explicit_type.kind = TYPE_UNKNOWN;
    explicit_type.name = NULL;
    explicit_type.explicitly_typed = false;
    if (is_type_token(parser->current.kind)) {
        /* Both `forge whole count = ...` and `forge count: whole = ...`
         * are accepted; neither introduces an alternate keyword vocabulary. */
        explicit_type = parse_type(parser);
    }
    if (!at(parser, TOKEN_IDENTIFIER)) {
        parser_report_current(parser, "expected a declaration name, found %s",
                              token_kind_name(parser->current.kind));
        free(explicit_type.name);
        return statement;
    }
    statement->as.var_decl.name = copy_token_text(&parser->current);
    if (statement->as.var_decl.name == NULL) {
        free(explicit_type.name);
        parser_report_current(parser, "out of memory copying declaration name");
        parser->fatal = true;
        parser->current.kind = TOKEN_EOF;
        return statement;
    }
    advance_token(parser);
    if (explicit_type.kind == TYPE_UNKNOWN && match(parser, TOKEN_COLON)) {
        explicit_type = parse_type(parser);
    } else if (explicit_type.kind == TYPE_UNKNOWN &&
               is_type_token(parser->current.kind)) {
        /* Accept `forge value whole = ...` in addition to the canonical
         * `forge value: whole = ...` spelling. */
        explicit_type = parse_type(parser);
    } else if (explicit_type.kind != TYPE_UNKNOWN &&
               match(parser, TOKEN_COLON)) {
        parser_report_current(parser,
                              "a declaration cannot have two type annotations");
    }
    statement->as.var_decl.type = explicit_type;
    if (!expect(parser, TOKEN_EQUAL, "'=' in declaration")) {
        return statement;
    }
    statement->as.var_decl.initializer = parse_expression(parser);
    if (statement->as.var_decl.initializer == NULL) {
        parser_report_current(parser, "expected an initializer expression");
    } else {
        statement->span = span_join(span, statement->as.var_decl.initializer->span);
    }
    return statement;
}

static AstBlock *parse_block_impl(Parser *parser);
static AstBlock *parse_block(Parser *parser) {
    AstBlock *result;
    if (!enter_recursion(parser)) return NULL;
    result = parse_block_impl(parser);
    --parser->recursion_depth;
    return result;
}

static AstBlock *parse_block_impl(Parser *parser) {
    SourceSpan start;
    AstBlock *block;

    if (!at(parser, TOKEN_LEFT_BRACE)) {
        parser_report_current(parser, "expected '{' to begin block");
        return NULL;
    }
    start = token_span(parser, &parser->current);
    advance_token(parser);
    block = ast_block_new(start);
    if (block == NULL) {
        parser_report(parser, start.start, "out of memory while creating block");
        return NULL;
    }
    consume_separators(parser);
    while (!at(parser, TOKEN_RIGHT_BRACE) && !at(parser, TOKEN_EOF)) {
        AstStmt *statement = parse_statement(parser);
        if (statement != NULL && !ast_block_add_statement(block, statement)) {
            parser_report(parser, statement->span.start,
                          "out of memory while adding statement");
            /* The statement is no longer owned by the block. */
            ast_stmt_free(statement);
            statement = NULL;
        }
        if (at(parser, TOKEN_SEMICOLON) || at(parser, TOKEN_NEWLINE)) {
            consume_separators(parser);
        } else if (!at(parser, TOKEN_RIGHT_BRACE) && !at(parser, TOKEN_EOF) &&
                   !statement_has_brace_boundary(statement)) {
            parser_report_current(parser, "expected ';' or a new line after statement");
            synchronize(parser);
        }
    }
    if (at(parser, TOKEN_RIGHT_BRACE)) {
        block->span = span_join(start, token_span(parser, &parser->current));
        advance_token(parser);
    } else {
        parser_report_current(parser, "expected '}' to close block");
        block->span = span_join(start, token_span(parser, &parser->current));
    }
    return block;
}

static AstStmt *parse_when(Parser *parser) {
    SourceSpan start = token_span(parser, &parser->current);
    AstStmt *statement = ast_stmt_new(AST_STMT_WHEN, start);
    if (statement == NULL) {
        parser_report(parser, start.start, "out of memory while creating when statement");
        return NULL;
    }
    advance_token(parser);
    statement->as.when_stmt.condition = parse_expression(parser);
    statement->as.when_stmt.then_branch = parse_block(parser);
    if (match(parser, TOKEN_KW_OTHERWISE)) {
        statement->as.when_stmt.else_branch = parse_block(parser);
    }
    if (statement->as.when_stmt.then_branch != NULL) {
        statement->span = span_join(start,
                                    statement->as.when_stmt.else_branch != NULL
                                        ? statement->as.when_stmt.else_branch->span
                                        : statement->as.when_stmt.then_branch->span);
    }
    return statement;
}

static AstStmt *parse_cycle(Parser *parser) {
    SourceSpan start = token_span(parser, &parser->current);
    AstStmt *statement = ast_stmt_new(AST_STMT_CYCLE, start);
    if (statement == NULL) {
        parser_report(parser, start.start, "out of memory while creating cycle statement");
        return NULL;
    }
    advance_token(parser);
    statement->as.cycle.condition = parse_expression(parser);
    statement->as.cycle.body = parse_block(parser);
    if (statement->as.cycle.body != NULL) {
        statement->span = span_join(start, statement->as.cycle.body->span);
    }
    return statement;
}

static AstStmt *parse_traverse(Parser *parser) {
    SourceSpan start = token_span(parser, &parser->current);
    AstStmt *statement = ast_stmt_new(AST_STMT_TRAVERSE, start);
    AstStmt *binding;
    if (statement == NULL) {
        parser_report(parser, start.start, "out of memory while creating traverse statement");
        return NULL;
    }
    advance_token(parser);
    if (!at(parser, TOKEN_IDENTIFIER)) {
        parser_report_current(parser, "expected an iteration binding name after traverse");
        return statement;
    }
    binding = ast_stmt_new(AST_STMT_VAR_DECL, token_span(parser, &parser->current));
    if (binding == NULL) {
        parser_report(parser, parser->current.location,
                      "out of memory while creating traverse binding");
        return statement;
    }
    statement->as.traverse.binding = binding;
    binding->as.var_decl.storage = AST_STORAGE_FORGE;
    binding->as.var_decl.name = copy_token_text(&parser->current);
    if (binding->as.var_decl.name == NULL) {
        parser_report(parser, parser->current.location,
                      "out of memory copying traverse binding name");
        return statement;
    }
    advance_token(parser);
    if (!expect(parser, TOKEN_KW_IN, "'in' after traverse binding")) {
        return statement;
    }
    statement->as.traverse.iterable = parse_expression(parser);
    statement->as.traverse.body = parse_block(parser);
    if (statement->as.traverse.body != NULL) {
        statement->span = span_join(start, statement->as.traverse.body->span);
    }
    return statement;
}

static AstStmt *parse_statement(Parser *parser) {
    Token token = parser->current;
    AstStmt *statement;

    switch (token.kind) {
    case TOKEN_KW_FORGE:
    case TOKEN_KW_SHIFT:
    case TOKEN_KW_ANCHOR:
        return parse_declaration(parser);
    case TOKEN_KW_YIELD: {
        SourceSpan start = token_span(parser, &token);
        advance_token(parser);
        statement = ast_stmt_new(AST_STMT_YIELD, start);
        if (statement == NULL) {
            parser_report(parser, token.location, "out of memory while creating yield");
            return NULL;
        }
        if (!at(parser, TOKEN_NEWLINE) && !at(parser, TOKEN_SEMICOLON) &&
            !at(parser, TOKEN_RIGHT_BRACE) && !at(parser, TOKEN_EOF)) {
            statement->as.yield.expression = parse_expression(parser);
            if (statement->as.yield.expression != NULL) {
                statement->span = span_join(start, statement->as.yield.expression->span);
            }
        }
        return statement;
    }
    case TOKEN_KW_WHEN:
        return parse_when(parser);
    case TOKEN_KW_CYCLE:
        return parse_cycle(parser);
    case TOKEN_KW_TRAVERSE:
        return parse_traverse(parser);
    case TOKEN_KW_ESCAPE:
        statement = ast_stmt_new(AST_STMT_ESCAPE, token_span(parser, &token));
        advance_token(parser);
        return statement;
    case TOKEN_KW_SKIP:
        statement = ast_stmt_new(AST_STMT_SKIP, token_span(parser, &token));
        advance_token(parser);
        return statement;
    case TOKEN_LEFT_BRACE:
        statement = ast_stmt_new(AST_STMT_BLOCK, token_span(parser, &token));
        if (statement != NULL) {
            statement->as.block.block = parse_block(parser);
            if (statement->as.block.block != NULL) {
                statement->span = statement->as.block.block->span;
            }
        }
        return statement;
    case TOKEN_KW_SUMMON:
        parser_report_current(parser, "summon is only valid in the top-level preamble");
        advance_token(parser);
        return NULL;
    case TOKEN_KW_IN:
        parser_report_current(parser, "keyword %s is not supported in this language slice",
                              token_kind_name(token.kind));
        advance_token(parser);
        return NULL;
    default: {
        AstExpr *expression = parse_expression(parser);
        if (expression == NULL) {
            return NULL;
        }
        statement = ast_stmt_new(AST_STMT_EXPRESSION, expression->span);
        if (statement == NULL) {
            parser_report(parser, expression->span.start,
                          "out of memory while creating expression statement");
            ast_expr_free(expression);
            return NULL;
        }
        statement->as.expression.expression = expression;
        return statement;
    }
    }
}

static AstFunction *parse_function(Parser *parser) {
    Token orbit = parser->current;
    SourceSpan start = token_span(parser, &orbit);
    AstFunction *function;
    char *function_name;

    advance_token(parser);
    if (!at(parser, TOKEN_IDENTIFIER)) {
        parser_report_current(parser, "expected orbit name, found %s",
                              token_kind_name(parser->current.kind));
        return NULL;
    }
    function_name = copy_token_text(&parser->current);
    if (function_name == NULL) {
        parser_report(parser, parser->current.location,
                      "out of memory while copying orbit name");
        return NULL;
    }
    function = ast_function_new(start, function_name);
    free(function_name);
    advance_token(parser);
    if (function == NULL) {
        parser_report(parser, orbit.location,
                      "out of memory while creating orbit function");
        return NULL;
    }
    if (!expect(parser, TOKEN_LEFT_PAREN, "'(' after orbit name")) {
        return function;
    }
    if (!at(parser, TOKEN_RIGHT_PAREN)) {
        for (;;) {
            AstParameter parameter;
            Token name_token;
            memset(&parameter, 0, sizeof(parameter));
            parameter.type.kind = TYPE_UNKNOWN;
            parameter.type.name = NULL;
            parameter.type.explicitly_typed = false;
            if (is_type_token(parser->current.kind)) {
                parameter.type = parse_type(parser);
            }
            if (!at(parser, TOKEN_IDENTIFIER)) {
                parser_report_current(parser, "expected parameter name");
                free(parameter.type.name);
                break;
            }
            name_token = parser->current;
            parameter.span = token_span(parser, &name_token);
            parameter.name = copy_token_text(&name_token);
            if (parameter.name == NULL) {
                free(parameter.type.name);
                parser_report(parser, name_token.location, "out of memory copying parameter name");
                break;
            }
            advance_token(parser);
            if (parameter.type.kind == TYPE_UNKNOWN && match(parser, TOKEN_COLON)) {
                parameter.type = parse_type(parser);
            } else if (parameter.type.kind == TYPE_UNKNOWN &&
                       is_type_token(parser->current.kind)) {
                parameter.type = parse_type(parser);
            } else if (parameter.type.kind == TYPE_UNKNOWN) {
                parser_report_current(parser,
                                      "expected parameter type after '%s'",
                                      parameter.name != NULL ? parameter.name : "parameter");
            }
            if (!ast_function_add_parameter(function, parameter)) {
                free(parameter.name);
                free(parameter.type.name);
                parser_report(parser, name_token.location,
                              "out of memory while adding parameter");
                break;
            }
            if (!match(parser, TOKEN_COMMA) || at(parser, TOKEN_RIGHT_PAREN)) {
                break;
            }
        }
    }
    (void)expect(parser, TOKEN_RIGHT_PAREN, "')' after parameters");
    if (!expect(parser, TOKEN_MINUS, "'->' after parameters")) {
        return function;
    }
    if (!expect(parser, TOKEN_GREATER, "'->' after parameters")) {
        return function;
    }
    function->return_type = parse_type(parser);
    function->body = parse_block(parser);
    if (function->body != NULL) {
        function->span = span_join(start, function->body->span);
    }
    return function;
}

static void merge_lexer_diagnostics(Parser *parser) {
    size_t i;
    if (parser->diagnostics == NULL) {
        return;
    }
    for (i = 0u; i < lexer_diagnostic_count(&parser->lexer); ++i) {
        const Diagnostic *diagnostic = lexer_diagnostic_at(&parser->lexer, i);
        if (diagnostic != NULL) {
            (void)diagnostics_add(parser->diagnostics, diagnostic->location,
                                  diagnostic->message != NULL
                                      ? diagnostic->message
                                      : "lexical error");
        }
    }
}

AstProgram *parser_parse(const char *filename, const char *source,
                         Diagnostics *diagnostics) {
    Parser parser;
    AstProgram *program;
    bool preamble = true;

    if (source == NULL) {
        source = "";
    }
    memset(&parser, 0, sizeof(parser));
    parser.filename = filename != NULL ? filename : "<source>";
    parser.source = source;
    parser.diagnostics = diagnostics;
    program = ast_program_new(parser.filename);
    if (program == NULL) {
        if (diagnostics != NULL) {
            SourceLocation location = {parser.filename, 0u, 1u, 1u};
            (void)diagnostics_add(diagnostics, location,
                                  "out of memory while creating program");
        }
        return NULL;
    }
    parser.program = program;
    parser.filename = program->filename;
    lexer_init(&parser.lexer, parser.filename, source);
    parser.current = lexer_next(&parser.lexer);
    consume_separators(&parser);
    while (!at(&parser, TOKEN_EOF)) {
        AstTopLevel item;
        memset(&item, 0, sizeof(item));
        if (at(&parser, TOKEN_KW_SUMMON)) {
            SourceSpan start = token_span(&parser, &parser.current);
            AstStmt *statement;
            if (!preamble) {
                parser_report_current(&parser, "summon must precede declarations and other top-level work");
            }
            advance_token(&parser);
            if (!at(&parser, TOKEN_STRING_LITERAL)) {
                parser_report_current(&parser, "expected a text module path after summon");
                synchronize_top_level(&parser);
                continue;
            }
            statement = ast_stmt_new(AST_STMT_SUMMON,
                span_join(start, token_span(&parser, &parser.current)));
            if (statement == NULL) {
                parser_report(&parser, start.start, "out of memory creating summon");
                break;
            }
            statement->as.summon.path = copy_string_token(&parser.current,
                &statement->as.summon.path_length);
            if (statement->as.summon.path == NULL) {
                parser_report(&parser, start.start, "out of memory copying module path");
            }
            advance_token(&parser);
            item.kind = AST_TOP_EXPRESSION;
            item.as.statement = statement;
            item.span = statement->span;
        } else if (at(&parser, TOKEN_KW_ORBIT)) {
            preamble = false;
            item.kind = AST_TOP_FUNCTION;
            item.as.function = parse_function(&parser);
            if (item.as.function == NULL) {
                synchronize_top_level(&parser);
                continue;
            }
            item.span = item.as.function->span;
        } else if (at(&parser, TOKEN_KW_FORGE) || at(&parser, TOKEN_KW_SHIFT) ||
                   at(&parser, TOKEN_KW_ANCHOR)) {
            preamble = false;
            item.kind = AST_TOP_DECLARATION;
            item.as.statement = parse_declaration(&parser);
            if (item.as.statement == NULL) {
                synchronize_top_level(&parser);
                continue;
            }
            item.span = item.as.statement->span;
        } else {
            preamble = false;
            item.kind = AST_TOP_EXPRESSION;
            item.as.statement = parse_statement(&parser);
            if (item.as.statement == NULL) {
                synchronize_top_level(&parser);
                continue;
            }
            item.span = item.as.statement->span;
        }
        if (!ast_program_add_item(program, item)) {
            parser_report(&parser, item.span.start,
                          "out of memory while adding top-level item");
            if (item.kind == AST_TOP_FUNCTION) {
                /* The program cannot own this item after a failed append. */
                ast_function_free(item.as.function);
            } else {
                ast_stmt_free(item.as.statement);
            }
            break;
        }
        if (at(&parser, TOKEN_SEMICOLON) || at(&parser, TOKEN_NEWLINE)) {
            consume_separators(&parser);
        } else if (!at(&parser, TOKEN_EOF) &&
                   !(item.kind == AST_TOP_FUNCTION)) {
            parser_report_current(&parser,
                                  "expected ';' or a new line after top-level item");
            synchronize_top_level(&parser);
        }
    }
    if (lexer_has_errors(&parser.lexer) ||
        (diagnostics != NULL && diagnostics->allocation_failed)) {
        parser.had_error = true;
    }
    merge_lexer_diagnostics(&parser);
    program->had_parse_error = parser.had_error;
    lexer_destroy(&parser.lexer);
    return program;
}

AstProgram *parser_parse_source(const char *filename, const char *source,
                                Diagnostics *diagnostics) {
    return parser_parse(filename, source, diagnostics);
}
