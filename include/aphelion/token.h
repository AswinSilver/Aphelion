#ifndef APHELION_TOKEN_H
#define APHELION_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#include "aphelion/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TokenKind {
    TOKEN_EOF = 0,
    TOKEN_ERROR,
    TOKEN_IDENTIFIER,
    TOKEN_WHOLE_LITERAL,
    TOKEN_DECIMAL_LITERAL,
    TOKEN_STRING_LITERAL,
    TOKEN_GLYPH_LITERAL,
    TOKEN_NEWLINE,

    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_EQUAL_EQUAL,
    TOKEN_NOT_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_EQUAL,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_AND_AND,
    TOKEN_OR_OR,
    TOKEN_BANG,

    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET,
    TOKEN_RIGHT_BRACKET,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_SEMICOLON,
    TOKEN_DOT,

    TOKEN_KW_FORGE,
    TOKEN_KW_SHIFT,
    TOKEN_KW_ANCHOR,
    TOKEN_KW_ORBIT,
    TOKEN_KW_YIELD,
    TOKEN_KW_WHEN,
    TOKEN_KW_OTHERWISE,
    TOKEN_KW_CYCLE,
    TOKEN_KW_TRAVERSE,
    TOKEN_KW_ESCAPE,
    TOKEN_KW_SKIP,
    TOKEN_KW_SUMMON,
    TOKEN_KW_BRIGHT,
    TOKEN_KW_DIM,
    TOKEN_KW_HOLLOW,
    TOKEN_KW_REVEAL,
    TOKEN_KW_RECEIVE,
    TOKEN_KW_WHOLE,
    TOKEN_KW_DECIMAL,
    TOKEN_KW_SIGNAL,
    TOKEN_KW_TEXT,
    TOKEN_KW_GLYPH,
    TOKEN_KW_SILENT,
    TOKEN_KW_AND,
    TOKEN_KW_OR,
    TOKEN_KW_NOT,
    TOKEN_KW_IN
} TokenKind;

/* Convenient spellings for clients that do not use the KW_ prefix. */
#define TOKEN_FORGE TOKEN_KW_FORGE
#define TOKEN_SHIFT TOKEN_KW_SHIFT
#define TOKEN_ANCHOR TOKEN_KW_ANCHOR
#define TOKEN_ORBIT TOKEN_KW_ORBIT
#define TOKEN_YIELD TOKEN_KW_YIELD
#define TOKEN_WHEN TOKEN_KW_WHEN
#define TOKEN_OTHERWISE TOKEN_KW_OTHERWISE
#define TOKEN_CYCLE TOKEN_KW_CYCLE
#define TOKEN_TRAVERSE TOKEN_KW_TRAVERSE
#define TOKEN_ESCAPE TOKEN_KW_ESCAPE
#define TOKEN_SKIP TOKEN_KW_SKIP
#define TOKEN_SUMMON TOKEN_KW_SUMMON
#define TOKEN_BRIGHT TOKEN_KW_BRIGHT
#define TOKEN_DIM TOKEN_KW_DIM
#define TOKEN_HOLLOW TOKEN_KW_HOLLOW
#define TOKEN_REVEAL TOKEN_KW_REVEAL
#define TOKEN_RECEIVE TOKEN_KW_RECEIVE
#define TOKEN_WHOLE TOKEN_KW_WHOLE
#define TOKEN_DECIMAL TOKEN_KW_DECIMAL
#define TOKEN_SIGNAL TOKEN_KW_SIGNAL
#define TOKEN_TEXT TOKEN_KW_TEXT
#define TOKEN_GLYPH TOKEN_KW_GLYPH
#define TOKEN_SILENT TOKEN_KW_SILENT
#define TOKEN_AND TOKEN_KW_AND
#define TOKEN_OR TOKEN_KW_OR
#define TOKEN_NOT TOKEN_KW_NOT
#define TOKEN_IN TOKEN_KW_IN

/* Common alternate names for literal and delimiter kinds. */
#define TOKEN_INTEGER_LITERAL TOKEN_WHOLE_LITERAL
#define TOKEN_DECIMAL_NUMBER TOKEN_DECIMAL_LITERAL
#define TOKEN_STRING TOKEN_STRING_LITERAL
#define TOKEN_CHAR_LITERAL TOKEN_GLYPH_LITERAL
#define TOKEN_CHARACTER_LITERAL TOKEN_GLYPH_LITERAL
#define TOKEN_LPAREN TOKEN_LEFT_PAREN
#define TOKEN_RPAREN TOKEN_RIGHT_PAREN
#define TOKEN_LBRACE TOKEN_LEFT_BRACE
#define TOKEN_RBRACE TOKEN_RIGHT_BRACE
#define TOKEN_LBRACKET TOKEN_LEFT_BRACKET
#define TOKEN_RBRACKET TOKEN_RIGHT_BRACKET

typedef struct Token {
    TokenKind kind;
    SourceLocation location;

    /* Borrowed source span.  It is not NUL-terminated; the caller owns it. */
    const char *lexeme;
    size_t lexeme_length;

    /* These fields are meaningful for the corresponding literal kinds. */
    int64_t whole_value;
    int whole_min_magnitude; /* 2^63 is only valid immediately after unary '-' */
    double decimal_value;
    int glyph_value;
} Token;

const char *token_kind_name(TokenKind kind);

/* Return a keyword kind, or TOKEN_IDENTIFIER when text is not a keyword. */
TokenKind token_keyword_kind(const char *text, size_t length);
int token_kind_is_keyword(TokenKind kind);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_TOKEN_H */
