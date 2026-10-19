#include "aphelion/lexer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_identifier_start(char c) {
    return c == '_' || is_ascii_alpha(c);
}

static int is_identifier_continue(char c) {
    return is_identifier_start(c) || is_ascii_digit(c);
}

static int is_hex_digit(char c) {
    return is_ascii_digit(c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_digit_value(char c) {
    if (is_ascii_digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

static int is_line_break(char c) {
    return c == '\n' || c == '\r';
}

static int is_horizontal_space(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static SourceLocation lexer_location(const Lexer *lexer) {
    SourceLocation location;
    location.filename = lexer->filename;
    location.offset = lexer->offset;
    location.line = lexer->line;
    location.column = lexer->column;
    return location;
}

static void consume_line_break(Lexer *lexer) {
    if (*lexer->current == '\r') {
        ++lexer->current;
        ++lexer->offset;
        if (*lexer->current == '\n') {
            ++lexer->current;
            ++lexer->offset;
        }
    } else if (*lexer->current == '\n') {
        ++lexer->current;
        ++lexer->offset;
    } else {
        return;
    }
    ++lexer->line;
    lexer->column = 1u;
}

/* Consume one source byte, treating CRLF as one line break. */
static void consume_byte(Lexer *lexer) {
    if (*lexer->current == '\0') {
        return;
    }
    if (is_line_break(*lexer->current)) {
        consume_line_break(lexer);
        return;
    }
    ++lexer->current;
    ++lexer->offset;
    ++lexer->column;
}

static Token make_token(TokenKind kind,
                        const char *start,
                        size_t length,
                        SourceLocation location) {
    Token token;
    memset(&token, 0, sizeof(token));
    token.kind = kind;
    token.location = location;
    token.lexeme = start;
    token.lexeme_length = length;
    return token;
}

static Token make_error(Lexer *lexer,
                        const char *start,
                        size_t length,
                        SourceLocation location,
                        const char *message) {
    (void)diagnostics_add(&lexer->diagnostics, location, message);
    return make_token(TOKEN_ERROR, start, length, location);
}

static int diagnostics_reserve(Diagnostics *diagnostics, size_t needed) {
    size_t new_capacity;
    Diagnostic *new_items;

    if (needed <= diagnostics->capacity) {
        return 1;
    }

    new_capacity = diagnostics->capacity == 0u ? 8u : diagnostics->capacity;
    while (new_capacity < needed) {
        if (new_capacity > (size_t)-1 / 2u) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2u;
    }
    if (new_capacity > (size_t)-1 / sizeof(*diagnostics->items)) {
        return 0;
    }

    new_items = (Diagnostic *)realloc(diagnostics->items,
                                       new_capacity * sizeof(*new_items));
    if (new_items == NULL) {
        return 0;
    }
    diagnostics->items = new_items;
    diagnostics->capacity = new_capacity;
    return 1;
}

static int diagnostics_add_owned(Diagnostics *diagnostics,
                                 SourceLocation location,
                                 char *message) {
    if (diagnostics == NULL || message == NULL) {
        free(message);
        return 0;
    }
    if (diagnostics->count == (size_t)-1 ||
        !diagnostics_reserve(diagnostics, diagnostics->count + 1u)) {
        free(message);
        diagnostics->allocation_failed = 1;
        return 0;
    }
    diagnostics->items[diagnostics->count].location = location;
    diagnostics->items[diagnostics->count].message = message;
    ++diagnostics->count;
    return 1;
}

void diagnostics_init(Diagnostics *diagnostics) {
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->items = NULL;
    diagnostics->count = 0u;
    diagnostics->capacity = 0u;
    diagnostics->allocation_failed = 0;
}

void diagnostics_clear(Diagnostics *diagnostics) {
    size_t i;

    if (diagnostics == NULL) {
        return;
    }
    for (i = 0u; i < diagnostics->count; ++i) {
        free(diagnostics->items[i].message);
        diagnostics->items[i].message = NULL;
    }
    diagnostics->count = 0u;
    diagnostics->allocation_failed = 0;
}

void diagnostics_free(Diagnostics *diagnostics) {
    if (diagnostics == NULL) {
        return;
    }
    diagnostics_clear(diagnostics);
    free(diagnostics->items);
    diagnostics->items = NULL;
    diagnostics->capacity = 0u;
}

int diagnostics_add(Diagnostics *diagnostics,
                    SourceLocation location,
                    const char *message) {
    size_t length;
    char *copy;

    if (diagnostics == NULL || message == NULL) {
        return 0;
    }
    length = strlen(message);
    if (length == (size_t)-1) {
        return 0;
    }
    if (length > (size_t)-1 - 1u) {
        return 0;
    }
    copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        diagnostics->allocation_failed = 1;
        return 0;
    }
    memcpy(copy, message, length + 1u);
    return diagnostics_add_owned(diagnostics, location, copy);
}

int diagnostics_addf(Diagnostics *diagnostics,
                     SourceLocation location,
                     const char *format,
                     ...) {
    va_list arguments;
    va_list sizing_arguments;
    int required;
    char *message;

    if (diagnostics == NULL || format == NULL) {
        return 0;
    }

    va_start(arguments, format);
    va_copy(sizing_arguments, arguments);
    required = vsnprintf(NULL, 0u, format, sizing_arguments);
    va_end(sizing_arguments);
    if (required < 0) {
        va_end(arguments);
        return 0;
    }
    if ((size_t)required > (size_t)-1 - 1u) {
        va_end(arguments);
        return 0;
    }
    message = (char *)malloc((size_t)required + 1u);
    if (message == NULL) {
        diagnostics->allocation_failed = 1;
        va_end(arguments);
        return 0;
    }
    (void)vsnprintf(message, (size_t)required + 1u, format, arguments);
    va_end(arguments);
    return diagnostics_add_owned(diagnostics, location, message);
}

size_t diagnostics_count(const Diagnostics *diagnostics) {
    return diagnostics == NULL ? 0u : diagnostics->count;
}

const Diagnostic *diagnostics_at(const Diagnostics *diagnostics, size_t index) {
    if (diagnostics == NULL || index >= diagnostics->count) {
        return NULL;
    }
    return &diagnostics->items[index];
}

int diagnostics_has_errors(const Diagnostics *diagnostics) {
    return diagnostics != NULL &&
           (diagnostics->count != 0u || diagnostics->allocation_failed);
}

void lexer_init(Lexer *lexer, const char *filename, const char *source) {
    if (lexer == NULL) {
        return;
    }
    lexer->filename = filename == NULL ? "<source>" : filename;
    lexer->source = source == NULL ? "" : source;
    lexer->current = lexer->source;
    lexer->offset = 0u;
    lexer->line = 1u;
    lexer->column = 1u;
    diagnostics_init(&lexer->diagnostics);
}

void lexer_destroy(Lexer *lexer) {
    if (lexer == NULL) {
        return;
    }
    diagnostics_free(&lexer->diagnostics);
}

void lexer_dispose(Lexer *lexer) {
    lexer_destroy(lexer);
}

const Diagnostics *lexer_diagnostics(const Lexer *lexer) {
    return lexer == NULL ? NULL : &lexer->diagnostics;
}

size_t lexer_diagnostic_count(const Lexer *lexer) {
    return lexer == NULL ? 0u : diagnostics_count(&lexer->diagnostics);
}

const Diagnostic *lexer_diagnostic_at(const Lexer *lexer, size_t index) {
    return lexer == NULL ? NULL : diagnostics_at(&lexer->diagnostics, index);
}

int lexer_has_errors(const Lexer *lexer) {
    return lexer != NULL && diagnostics_has_errors(&lexer->diagnostics);
}

void lexer_clear_diagnostics(Lexer *lexer) {
    if (lexer != NULL) {
        diagnostics_clear(&lexer->diagnostics);
    }
}

static int skip_ignored(Lexer *lexer, Token *error_token) {
    for (;;) {
        while (is_horizontal_space(*lexer->current)) {
            consume_byte(lexer);
        }

        if (lexer->current[0] == '/' && lexer->current[1] == '/') {
            consume_byte(lexer);
            consume_byte(lexer);
            while (*lexer->current != '\0' &&
                   !is_line_break(*lexer->current)) {
                consume_byte(lexer);
            }
            continue;
        }

        if (lexer->current[0] == '/' && lexer->current[1] == '*') {
            const char *comment_start = lexer->current;
            SourceLocation comment_location = lexer_location(lexer);
            consume_byte(lexer);
            consume_byte(lexer);
            while (*lexer->current != '\0' &&
                   !(lexer->current[0] == '*' && lexer->current[1] == '/')) {
                consume_byte(lexer);
            }
            if (*lexer->current == '\0') {
                *error_token = make_error(lexer,
                                          comment_start,
                                          (size_t)(lexer->current - comment_start),
                                          comment_location,
                                          "unterminated block comment");
                return 1;
            }
            consume_byte(lexer);
            consume_byte(lexer);
            continue;
        }
        return 0;
    }
}

static void consume_identifier_tail(Lexer *lexer) {
    while (is_identifier_continue(*lexer->current)) {
        consume_byte(lexer);
    }
}

static void consume_invalid_numeric_tail(Lexer *lexer) {
    while (is_identifier_continue(*lexer->current) ||
           *lexer->current == '.') {
        consume_byte(lexer);
    }
}

static Token lex_identifier(Lexer *lexer) {
    const char *start = lexer->current;
    SourceLocation location = lexer_location(lexer);
    TokenKind kind;

    consume_byte(lexer);
    consume_identifier_tail(lexer);
    kind = token_keyword_kind(start, (size_t)(lexer->current - start));
    return make_token(kind,
                      start,
                      (size_t)(lexer->current - start),
                      location);
}

static Token lex_number(Lexer *lexer) {
    const char *start = lexer->current;
    SourceLocation location = lexer_location(lexer);
    int is_decimal = 0;
    int dot_belongs_to_number = 0;
    int malformed = 0;
    char *end;
    errno = 0;

    if (*lexer->current == '.') {
        is_decimal = 1;
        dot_belongs_to_number = 1;
        consume_byte(lexer);
        while (is_ascii_digit(*lexer->current)) {
            consume_byte(lexer);
        }
    } else {
        while (is_ascii_digit(*lexer->current)) {
            consume_byte(lexer);
        }
        if (*lexer->current == '.' && is_ascii_digit(lexer->current[1])) {
            is_decimal = 1;
            dot_belongs_to_number = 1;
            consume_byte(lexer);
            while (is_ascii_digit(*lexer->current)) {
                consume_byte(lexer);
            }
        } else if (*lexer->current == '.' && lexer->current[1] == '.') {
            malformed = 1;
            consume_invalid_numeric_tail(lexer);
        }
    }

    if (*lexer->current == 'e' || *lexer->current == 'E') {
        is_decimal = 1;
        consume_byte(lexer);
        if (*lexer->current == '+' || *lexer->current == '-') {
            consume_byte(lexer);
        }
        if (!is_ascii_digit(*lexer->current)) {
            malformed = 1;
        } else {
            while (is_ascii_digit(*lexer->current)) {
                consume_byte(lexer);
            }
        }
    }

    if (is_identifier_continue(*lexer->current)) {
        malformed = 1;
        consume_identifier_tail(lexer);
    }
    if (dot_belongs_to_number && *lexer->current == '.') {
        malformed = 1;
        consume_invalid_numeric_tail(lexer);
    }

    if (malformed) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "malformed numeric literal");
    }

    if (is_decimal) {
        Token token = make_token(TOKEN_DECIMAL_LITERAL,
                                 start,
                                 (size_t)(lexer->current - start),
                                 location);
        double value;
        errno = 0;
        value = strtod(start, &end);
        if (errno == ERANGE || end != lexer->current) {
            return make_error(lexer,
                              start,
                              (size_t)(lexer->current - start),
                              location,
                              "decimal literal is out of range");
        }
        token.decimal_value = value;
        return token;
    } else {
        Token token = make_token(TOKEN_WHOLE_LITERAL,
                                 start,
                                 (size_t)(lexer->current - start),
                                 location);
        uintmax_t value;
        errno = 0;
        value = strtoumax(start, &end, 10);
        if (errno == ERANGE || end != lexer->current ||
            value > (uintmax_t)INT64_MAX + 1u) {
            return make_error(lexer,
                              start,
                              (size_t)(lexer->current - start),
                              location,
                              "whole literal is out of range");
        }
        token.whole_min_magnitude = value == (uintmax_t)INT64_MAX + 1u;
        token.whole_value = token.whole_min_magnitude ? INT64_MIN : (int64_t)value;
        return token;
    }
}

/* Consume the character after a backslash and decode it for glyph literals. */
static int consume_escape(Lexer *lexer, int *value) {
    int first;

    if (*lexer->current == '\0' || is_line_break(*lexer->current)) {
        return 0;
    }
    switch (*lexer->current) {
    case 'a':
        *value = '\a';
        consume_byte(lexer);
        return 1;
    case 'b':
        *value = '\b';
        consume_byte(lexer);
        return 1;
    case 'f':
        *value = '\f';
        consume_byte(lexer);
        return 1;
    case 'n':
        *value = '\n';
        consume_byte(lexer);
        return 1;
    case 'r':
        *value = '\r';
        consume_byte(lexer);
        return 1;
    case 't':
        *value = '\t';
        consume_byte(lexer);
        return 1;
    case 'v':
        *value = '\v';
        consume_byte(lexer);
        return 1;
    case '0':
        *value = 0;
        consume_byte(lexer);
        return 1;
    case '\\':
        *value = '\\';
        consume_byte(lexer);
        return 1;
    case '\'':
        *value = '\'';
        consume_byte(lexer);
        return 1;
    case '"':
        *value = '"';
        consume_byte(lexer);
        return 1;
    case 'x':
        consume_byte(lexer);
        if (!is_hex_digit(*lexer->current)) {
            return 0;
        }
        first = hex_digit_value(*lexer->current);
        consume_byte(lexer);
        if (!is_hex_digit(*lexer->current)) {
            return 0;
        }
        *value = first * 16 + hex_digit_value(*lexer->current);
        consume_byte(lexer);
        return 1;
    default:
        consume_byte(lexer);
        return 0;
    }
}

static Token lex_string(Lexer *lexer) {
    const char *start = lexer->current;
    SourceLocation location = lexer_location(lexer);
    int invalid_escape = 0;
    int closed = 0;

    consume_byte(lexer); /* opening quote */
    while (*lexer->current != '\0') {
        if (*lexer->current == '"') {
            consume_byte(lexer);
            closed = 1;
            break;
        }
        if (is_line_break(*lexer->current)) {
            break;
        }
        if (*lexer->current == '\\') {
            int ignored_value = 0;
            consume_byte(lexer);
            if (!consume_escape(lexer, &ignored_value)) {
                invalid_escape = 1;
            }
            continue;
        }
        if ((unsigned char)*lexer->current < 0x20u &&
            *lexer->current != '\t') {
            invalid_escape = 1;
        }
        consume_byte(lexer);
    }

    if (!closed) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "unterminated string literal");
    }
    if (invalid_escape) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "invalid escape in string literal");
    }
    return make_token(TOKEN_STRING_LITERAL,
                      start,
                      (size_t)(lexer->current - start),
                      location);
}

static Token lex_glyph(Lexer *lexer) {
    const char *start = lexer->current;
    SourceLocation location = lexer_location(lexer);
    int value = 0;
    int invalid = 0;
    int closed = 0;

    consume_byte(lexer); /* opening quote */
    if (*lexer->current == '\'') {
        consume_byte(lexer);
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "empty glyph literal");
    }
    if (*lexer->current == '\0' || is_line_break(*lexer->current)) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "unterminated glyph literal");
    }

    if (*lexer->current == '\\') {
        consume_byte(lexer);
        if (!consume_escape(lexer, &value)) {
            invalid = 1;
        }
    } else {
        if ((unsigned char)*lexer->current < 0x20u &&
            *lexer->current != '\t') {
            invalid = 1;
        }
        value = (unsigned char)*lexer->current;
        consume_byte(lexer);
    }

    if (*lexer->current == '\'') {
        consume_byte(lexer);
        closed = 1;
    } else {
        invalid = 1;
        while (*lexer->current != '\0' &&
               !is_line_break(*lexer->current)) {
            if (*lexer->current == '\'') {
                consume_byte(lexer);
                closed = 1;
                break;
            }
            if (*lexer->current == '\\') {
                int ignored_value = 0;
                consume_byte(lexer);
                if (!consume_escape(lexer, &ignored_value)) {
                    invalid = 1;
                }
            } else {
                consume_byte(lexer);
            }
        }
    }

    if (!closed) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "unterminated glyph literal");
    }
    if (invalid) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "invalid glyph literal");
    }

    {
        Token token = make_token(TOKEN_GLYPH_LITERAL,
                                 start,
                                 (size_t)(lexer->current - start),
                                 location);
        token.glyph_value = value;
        return token;
    }
}

static Token lex_operator_or_punctuation(Lexer *lexer) {
    const char *start = lexer->current;
    SourceLocation location = lexer_location(lexer);
    TokenKind kind = TOKEN_ERROR;
    int recognized = 1;

    switch (*lexer->current) {
    case '+':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_PLUS_EQUAL;
        } else {
            kind = TOKEN_PLUS;
        }
        break;
    case '-':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_MINUS_EQUAL;
        } else {
            kind = TOKEN_MINUS;
        }
        break;
    case '*':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_STAR_EQUAL;
        } else {
            kind = TOKEN_STAR;
        }
        break;
    case '/':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_SLASH_EQUAL;
        } else {
            kind = TOKEN_SLASH;
        }
        break;
    case '%':
        consume_byte(lexer);
        kind = TOKEN_PERCENT;
        break;
    case '=':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_EQUAL_EQUAL;
        } else {
            kind = TOKEN_EQUAL;
        }
        break;
    case '!':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_NOT_EQUAL;
        } else {
            kind = TOKEN_BANG;
        }
        break;
    case '<':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_LESS_EQUAL;
        } else {
            kind = TOKEN_LESS;
        }
        break;
    case '>':
        consume_byte(lexer);
        if (*lexer->current == '=') {
            consume_byte(lexer);
            kind = TOKEN_GREATER_EQUAL;
        } else {
            kind = TOKEN_GREATER;
        }
        break;
    case '&':
        consume_byte(lexer);
        if (*lexer->current == '&') {
            consume_byte(lexer);
            kind = TOKEN_AND_AND;
        } else {
            recognized = 0;
        }
        break;
    case '|':
        consume_byte(lexer);
        if (*lexer->current == '|') {
            consume_byte(lexer);
            kind = TOKEN_OR_OR;
        } else {
            recognized = 0;
        }
        break;
    case '(':
        consume_byte(lexer);
        kind = TOKEN_LEFT_PAREN;
        break;
    case ')':
        consume_byte(lexer);
        kind = TOKEN_RIGHT_PAREN;
        break;
    case '{':
        consume_byte(lexer);
        kind = TOKEN_LEFT_BRACE;
        break;
    case '}':
        consume_byte(lexer);
        kind = TOKEN_RIGHT_BRACE;
        break;
    case '[':
        consume_byte(lexer);
        kind = TOKEN_LEFT_BRACKET;
        break;
    case ']':
        consume_byte(lexer);
        kind = TOKEN_RIGHT_BRACKET;
        break;
    case ',':
        consume_byte(lexer);
        kind = TOKEN_COMMA;
        break;
    case ':':
        consume_byte(lexer);
        kind = TOKEN_COLON;
        break;
    case ';':
        consume_byte(lexer);
        kind = TOKEN_SEMICOLON;
        break;
    case '.':
        consume_byte(lexer);
        kind = TOKEN_DOT;
        break;
    default:
        consume_byte(lexer);
        recognized = 0;
        break;
    }

    if (!recognized) {
        return make_error(lexer,
                          start,
                          (size_t)(lexer->current - start),
                          location,
                          "invalid character or operator");
    }
    return make_token(kind,
                      start,
                      (size_t)(lexer->current - start),
                      location);
}

Token lexer_next(Lexer *lexer) {
    Token ignored_error;

    if (lexer == NULL) {
        Token token;
        memset(&token, 0, sizeof(token));
        token.kind = TOKEN_ERROR;
        return token;
    }

    if (skip_ignored(lexer, &ignored_error)) {
        return ignored_error;
    }

    if (*lexer->current == '\0') {
        SourceLocation location = lexer_location(lexer);
        return make_token(TOKEN_EOF, lexer->current, 0u, location);
    }
    if (is_line_break(*lexer->current)) {
        const char *start = lexer->current;
        SourceLocation location = lexer_location(lexer);
        consume_line_break(lexer);
        return make_token(TOKEN_NEWLINE,
                          start,
                          (size_t)(lexer->current - start),
                          location);
    }
    if (is_identifier_start(*lexer->current)) {
        return lex_identifier(lexer);
    }
    if (is_ascii_digit(*lexer->current) ||
        (*lexer->current == '.' && is_ascii_digit(lexer->current[1]))) {
        return lex_number(lexer);
    }
    if (*lexer->current == '"') {
        return lex_string(lexer);
    }
    if (*lexer->current == '\'') {
        return lex_glyph(lexer);
    }
    return lex_operator_or_punctuation(lexer);
}
