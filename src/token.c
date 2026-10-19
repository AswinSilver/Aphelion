#include "aphelion/token.h"

#include <string.h>

typedef struct KeywordEntry {
    const char *spelling;
    TokenKind kind;
} KeywordEntry;

static const KeywordEntry keyword_entries[] = {
    {"forge", TOKEN_KW_FORGE},
    {"shift", TOKEN_KW_SHIFT},
    {"anchor", TOKEN_KW_ANCHOR},
    {"orbit", TOKEN_KW_ORBIT},
    {"yield", TOKEN_KW_YIELD},
    {"when", TOKEN_KW_WHEN},
    {"otherwise", TOKEN_KW_OTHERWISE},
    {"cycle", TOKEN_KW_CYCLE},
    {"traverse", TOKEN_KW_TRAVERSE},
    {"escape", TOKEN_KW_ESCAPE},
    {"skip", TOKEN_KW_SKIP},
    {"summon", TOKEN_KW_SUMMON},
    {"bright", TOKEN_KW_BRIGHT},
    {"dim", TOKEN_KW_DIM},
    {"hollow", TOKEN_KW_HOLLOW},
    {"reveal", TOKEN_KW_REVEAL},
    {"receive", TOKEN_KW_RECEIVE},
    {"whole", TOKEN_KW_WHOLE},
    {"decimal", TOKEN_KW_DECIMAL},
    {"signal", TOKEN_KW_SIGNAL},
    {"text", TOKEN_KW_TEXT},
    {"glyph", TOKEN_KW_GLYPH},
    {"silent", TOKEN_KW_SILENT},
    {"and", TOKEN_KW_AND},
    {"or", TOKEN_KW_OR},
    {"not", TOKEN_KW_NOT},
    {"in", TOKEN_KW_IN}
};

static const size_t keyword_entry_count =
    sizeof(keyword_entries) / sizeof(keyword_entries[0]);

TokenKind token_keyword_kind(const char *text, size_t length) {
    size_t i;

    if (text == NULL) {
        return TOKEN_IDENTIFIER;
    }

    for (i = 0u; i < keyword_entry_count; ++i) {
        size_t spelling_length = strlen(keyword_entries[i].spelling);
        if (spelling_length == length &&
            memcmp(text, keyword_entries[i].spelling, length) == 0) {
            return keyword_entries[i].kind;
        }
    }

    return TOKEN_IDENTIFIER;
}

int token_kind_is_keyword(TokenKind kind) {
    return kind >= TOKEN_KW_FORGE && kind <= TOKEN_KW_IN;
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "<eof>";
    case TOKEN_ERROR:
        return "<error>";
    case TOKEN_IDENTIFIER:
        return "identifier";
    case TOKEN_WHOLE_LITERAL:
        return "whole";
    case TOKEN_DECIMAL_LITERAL:
        return "decimal";
    case TOKEN_STRING_LITERAL:
        return "text";
    case TOKEN_GLYPH_LITERAL:
        return "glyph";
    case TOKEN_NEWLINE:
        return "<newline>";
    case TOKEN_PLUS:
        return "+";
    case TOKEN_MINUS:
        return "-";
    case TOKEN_STAR:
        return "*";
    case TOKEN_SLASH:
        return "/";
    case TOKEN_PERCENT:
        return "%";
    case TOKEN_EQUAL_EQUAL:
        return "==";
    case TOKEN_NOT_EQUAL:
        return "!=";
    case TOKEN_LESS:
        return "<";
    case TOKEN_LESS_EQUAL:
        return "<=";
    case TOKEN_GREATER:
        return ">";
    case TOKEN_GREATER_EQUAL:
        return ">=";
    case TOKEN_EQUAL:
        return "=";
    case TOKEN_PLUS_EQUAL:
        return "+=";
    case TOKEN_MINUS_EQUAL:
        return "-=";
    case TOKEN_STAR_EQUAL:
        return "*=";
    case TOKEN_SLASH_EQUAL:
        return "/=";
    case TOKEN_AND_AND:
        return "&&";
    case TOKEN_OR_OR:
        return "||";
    case TOKEN_BANG:
        return "!";
    case TOKEN_LEFT_PAREN:
        return "(";
    case TOKEN_RIGHT_PAREN:
        return ")";
    case TOKEN_LEFT_BRACE:
        return "{";
    case TOKEN_RIGHT_BRACE:
        return "}";
    case TOKEN_LEFT_BRACKET:
        return "[";
    case TOKEN_RIGHT_BRACKET:
        return "]";
    case TOKEN_COMMA:
        return ",";
    case TOKEN_COLON:
        return ":";
    case TOKEN_SEMICOLON:
        return ";";
    case TOKEN_DOT:
        return ".";
    case TOKEN_KW_FORGE:
        return "forge";
    case TOKEN_KW_SHIFT:
        return "shift";
    case TOKEN_KW_ANCHOR:
        return "anchor";
    case TOKEN_KW_ORBIT:
        return "orbit";
    case TOKEN_KW_YIELD:
        return "yield";
    case TOKEN_KW_WHEN:
        return "when";
    case TOKEN_KW_OTHERWISE:
        return "otherwise";
    case TOKEN_KW_CYCLE:
        return "cycle";
    case TOKEN_KW_TRAVERSE:
        return "traverse";
    case TOKEN_KW_ESCAPE:
        return "escape";
    case TOKEN_KW_SKIP:
        return "skip";
    case TOKEN_KW_SUMMON:
        return "summon";
    case TOKEN_KW_BRIGHT:
        return "bright";
    case TOKEN_KW_DIM:
        return "dim";
    case TOKEN_KW_HOLLOW:
        return "hollow";
    case TOKEN_KW_REVEAL:
        return "reveal";
    case TOKEN_KW_RECEIVE:
        return "receive";
    case TOKEN_KW_WHOLE:
        return "whole";
    case TOKEN_KW_DECIMAL:
        return "decimal";
    case TOKEN_KW_SIGNAL:
        return "signal";
    case TOKEN_KW_TEXT:
        return "text";
    case TOKEN_KW_GLYPH:
        return "glyph";
    case TOKEN_KW_SILENT:
        return "silent";
    case TOKEN_KW_AND:
        return "and";
    case TOKEN_KW_OR:
        return "or";
    case TOKEN_KW_NOT:
        return "not";
    case TOKEN_KW_IN:
        return "in";
    default:
        return "<unknown>";
    }
}
