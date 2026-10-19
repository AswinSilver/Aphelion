#include "aphelion/value.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static AphelionValue value_with_type(AphelionValueType type) {
    AphelionValue value;
    memset(&value, 0, sizeof(value));
    value.type = type;
    return value;
}

AphelionValue aphelion_value_hollow(void) {
    return value_with_type(APHELION_VALUE_HOLLOW);
}

AphelionValue aphelion_value_silent(void) {
    return value_with_type(APHELION_VALUE_SILENT);
}

AphelionValue aphelion_value_whole(int64_t value) {
    AphelionValue result = value_with_type(APHELION_VALUE_WHOLE);
    result.as.whole = value;
    return result;
}

AphelionValue aphelion_value_decimal(double value) {
    AphelionValue result = value_with_type(APHELION_VALUE_DECIMAL);
    result.as.decimal = value;
    return result;
}

AphelionValue aphelion_value_signal(bool value) {
    AphelionValue result = value_with_type(APHELION_VALUE_SIGNAL);
    result.as.signal = value;
    return result;
}

AphelionValue aphelion_value_glyph(uint32_t value) {
    AphelionValue result = value_with_type(APHELION_VALUE_GLYPH);
    result.as.glyph = value;
    return result;
}

AphelionValue aphelion_value_function(uint32_t function_index) {
    AphelionValue result = value_with_type(APHELION_VALUE_FUNCTION);
    result.as.function = function_index;
    return result;
}

AphelionValue aphelion_value_text_n(const char *text, size_t length) {
    AphelionValue result = value_with_type(APHELION_VALUE_TEXT);
    AphelionText *owned;

    if (text == NULL) {
        text = "";
        length = 0u;
    }
    if (length > SIZE_MAX - sizeof(*owned) - 1u) {
        return aphelion_value_hollow();
    }
    owned = (AphelionText *)malloc(sizeof(*owned) + length + 1u);
    if (owned == NULL) {
        return aphelion_value_hollow();
    }
    owned->references = 1u;
    owned->length = length;
    if (length != 0u) {
        memcpy(owned->bytes, text, length);
    }
    owned->bytes[length] = '\0';
    result.as.text = owned;
    return result;
}

AphelionValue aphelion_value_text(const char *text) {
    return aphelion_value_text_n(text, text != NULL ? strlen(text) : 0u);
}

AphelionValue aphelion_value_list(AphelionValueType element_type,
                                  const AphelionValue *items, size_t length) {
    AphelionValue result = value_with_type(APHELION_VALUE_LIST);
    AphelionList *owned;
    size_t i;
    if ((element_type < APHELION_VALUE_WHOLE ||
         element_type > APHELION_VALUE_GLYPH) &&
        !(length == 0u && element_type == APHELION_VALUE_HOLLOW)) {
        return aphelion_value_hollow();
    }
    if (length > (SIZE_MAX - sizeof(*owned)) / sizeof(*items) ||
        (length != 0u && items == NULL)) return aphelion_value_hollow();
    for (i = 0u; i < length; ++i) {
        if (items[i].type != element_type ||
            (element_type == APHELION_VALUE_TEXT && items[i].as.text == NULL) ||
            (element_type == APHELION_VALUE_DECIMAL && !isfinite(items[i].as.decimal)) ||
            (element_type == APHELION_VALUE_GLYPH &&
             (items[i].as.glyph > 0x10ffffu ||
              (items[i].as.glyph >= 0xd800u && items[i].as.glyph <= 0xdfffu)))) {
            return aphelion_value_hollow();
        }
    }
    owned = (AphelionList *)malloc(sizeof(*owned) + length * sizeof(*items));
    if (owned == NULL) return aphelion_value_hollow();
    owned->references = 1u;
    owned->length = length;
    owned->element_type = element_type;
    for (i = 0u; i < length; ++i) owned->items[i] = aphelion_value_copy(items[i]);
    result.as.list = owned;
    return result;
}

AphelionValue aphelion_value_copy(AphelionValue value) {
    if (value.type == APHELION_VALUE_TEXT && value.as.text != NULL &&
        value.as.text->references != SIZE_MAX) {
        value.as.text->references += 1u;
    }
    if (value.type == APHELION_VALUE_LIST && value.as.list != NULL &&
        value.as.list->references != SIZE_MAX) {
        value.as.list->references += 1u;
    }
    return value;
}

AphelionValue aphelion_value_move(AphelionValue *value) {
    AphelionValue result;
    if (value == NULL) {
        return aphelion_value_hollow();
    }
    result = *value;
    *value = aphelion_value_hollow();
    return result;
}

void aphelion_value_destroy(AphelionValue *value) {
    if (value == NULL) {
        return;
    }
    if (value->type == APHELION_VALUE_TEXT && value->as.text != NULL) {
        if (value->as.text->references == SIZE_MAX) {
            /* A saturated count is pinned rather than risking an early free. */
        } else if (value->as.text->references > 1u) {
            value->as.text->references -= 1u;
        } else {
            free(value->as.text);
        }
    }
    if (value->type == APHELION_VALUE_LIST && value->as.list != NULL) {
        if (value->as.list->references == SIZE_MAX) {
            /* Match the pinned saturated text reference-count policy. */
        } else if (value->as.list->references > 1u) {
            value->as.list->references -= 1u;
        } else {
            size_t i;
            for (i = 0u; i < value->as.list->length; ++i) {
                aphelion_value_destroy(&value->as.list->items[i]);
            }
            free(value->as.list);
        }
    }
    *value = aphelion_value_hollow();
}

const char *aphelion_value_type_name(AphelionValueType type) {
    switch (type) {
    case APHELION_VALUE_HOLLOW:
        return "hollow";
    case APHELION_VALUE_WHOLE:
        return "whole";
    case APHELION_VALUE_DECIMAL:
        return "decimal";
    case APHELION_VALUE_SIGNAL:
        return "signal";
    case APHELION_VALUE_TEXT:
        return "text";
    case APHELION_VALUE_GLYPH:
        return "glyph";
    case APHELION_VALUE_FUNCTION:
        return "function";
    case APHELION_VALUE_SILENT:
        return "silent";
    case APHELION_VALUE_UNINITIALIZED:
        return "uninitialized";
    case APHELION_VALUE_LIST:
        return "list";
    default:
        return "<invalid>";
    }
}

bool aphelion_value_truthy(AphelionValue value) {
    switch (value.type) {
    case APHELION_VALUE_HOLLOW:
    case APHELION_VALUE_SILENT:
        return false;
    case APHELION_VALUE_SIGNAL:
        return value.as.signal;
    case APHELION_VALUE_WHOLE:
        return value.as.whole != 0;
    case APHELION_VALUE_DECIMAL:
        return value.as.decimal != 0.0 && !isnan(value.as.decimal);
    case APHELION_VALUE_TEXT:
        return value.as.text != NULL && value.as.text->length != 0u;
    case APHELION_VALUE_LIST:
        return value.as.list != NULL && value.as.list->length != 0u;
    case APHELION_VALUE_GLYPH:
        return value.as.glyph != 0u;
    case APHELION_VALUE_FUNCTION:
        return true;
    default:
        return false;
    }
}

static bool numeric_value(AphelionValue value, double *out, bool *whole) {
    if (value.type == APHELION_VALUE_WHOLE) {
        if (out != NULL) {
            *out = (double)value.as.whole;
        }
        if (whole != NULL) {
            *whole = true;
        }
        return true;
    }
    if (value.type == APHELION_VALUE_DECIMAL) {
        if (out != NULL) {
            *out = value.as.decimal;
        }
        if (whole != NULL) {
            *whole = false;
        }
        return true;
    }
    return false;
}

bool aphelion_value_equal(AphelionValue left, AphelionValue right) {
    double left_number;
    double right_number;
    bool left_whole;
    bool right_whole;

    if (left.type == APHELION_VALUE_WHOLE && right.type == APHELION_VALUE_WHOLE)
        return left.as.whole == right.as.whole;
    if (numeric_value(left, &left_number, &left_whole) &&
        numeric_value(right, &right_number, &right_whole)) {
        (void)left_whole;
        (void)right_whole;
        return left_number == right_number;
    }
    if (left.type != right.type) {
        return false;
    }
    switch (left.type) {
    case APHELION_VALUE_HOLLOW:
    case APHELION_VALUE_SILENT:
        return true;
    case APHELION_VALUE_SIGNAL:
        return left.as.signal == right.as.signal;
    case APHELION_VALUE_TEXT:
        if (left.as.text == NULL || right.as.text == NULL) {
            return left.as.text == right.as.text;
        }
        return left.as.text->length == right.as.text->length &&
               memcmp(left.as.text->bytes, right.as.text->bytes,
                      left.as.text->length) == 0;
    case APHELION_VALUE_GLYPH:
        return left.as.glyph == right.as.glyph;
    case APHELION_VALUE_FUNCTION:
        return left.as.function == right.as.function;
    case APHELION_VALUE_LIST: {
        size_t i;
        if (left.as.list == NULL || right.as.list == NULL) {
            return left.as.list == right.as.list;
        }
        if (left.as.list->length != right.as.list->length) return false;
        for (i = 0u; i < left.as.list->length; ++i) {
            if (!aphelion_value_equal(left.as.list->items[i], right.as.list->items[i])) {
                return false;
            }
        }
        return true;
    }
    case APHELION_VALUE_WHOLE:
    case APHELION_VALUE_DECIMAL:
    default:
        return false;
    }
}

bool aphelion_value_write(FILE *out, AphelionValue value) {
    int result;
    if (out == NULL) {
        return false;
    }
    switch (value.type) {
    case APHELION_VALUE_HOLLOW:
        result = fputs("hollow", out);
        break;
    case APHELION_VALUE_SILENT:
        result = fputs("silent", out);
        break;
    case APHELION_VALUE_WHOLE:
        result = fprintf(out, "%" PRId64, value.as.whole);
        break;
    case APHELION_VALUE_DECIMAL:
        result = fprintf(out, "%.17g", value.as.decimal);
        break;
    case APHELION_VALUE_SIGNAL:
        result = fputs(value.as.signal ? "bright" : "dim", out);
        break;
    case APHELION_VALUE_TEXT:
        if (value.as.text == NULL || value.as.text->length == 0u) {
            return !ferror(out);
        }
        return fwrite(value.as.text->bytes, 1u, value.as.text->length, out) ==
                   value.as.text->length && !ferror(out);
    case APHELION_VALUE_GLYPH:
        result = fprintf(out, "U+%04" PRIX32, value.as.glyph);
        break;
    case APHELION_VALUE_FUNCTION:
        result = fprintf(out, "<function #%" PRIu32 ">", value.as.function);
        break;
    case APHELION_VALUE_LIST: {
        char *text = aphelion_value_to_string(value);
        if (text == NULL) return false;
        result = fputs(text, out);
        free(text);
        break;
    }
    default:
        result = fputs("<invalid>", out);
        break;
    }
    return result >= 0 && !ferror(out);
}

static char *format_alloc(const char *format, ...) {
    va_list args;
    va_list copy;
    int length;
    char *result;

    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0u, format, args);
    va_end(args);
    if (length < 0) {
        va_end(copy);
        return NULL;
    }
    result = (char *)malloc((size_t)length + 1u);
    if (result != NULL) {
        (void)vsnprintf(result, (size_t)length + 1u, format, copy);
    }
    va_end(copy);
    return result;
}

static bool append_string(char **buffer, size_t *used, size_t *capacity,
                          const char *bytes, size_t length) {
    size_t needed, next;
    char *grown;
    if (length > SIZE_MAX - *used - 1u) return false;
    needed = *used + length + 1u;
    if (needed > *capacity) {
        next = *capacity != 0u ? *capacity : 32u;
        while (next < needed) {
            if (next > SIZE_MAX / 2u) { next = needed; break; }
            next *= 2u;
        }
        grown = (char *)realloc(*buffer, next);
        if (grown == NULL) return false;
        *buffer = grown;
        *capacity = next;
    }
    if (length != 0u) memcpy(*buffer + *used, bytes, length);
    *used += length;
    (*buffer)[*used] = '\0';
    return true;
}

static bool append_quoted_text(char **buffer, size_t *used, size_t *capacity,
                               const AphelionText *text) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (text == NULL || !append_string(buffer, used, capacity, "\"", 1u)) return false;
    for (i = 0u; i < text->length; ++i) {
        unsigned char byte = (unsigned char)text->bytes[i];
        char escaped[4];
        size_t length = 2u;
        escaped[0] = '\\';
        switch (byte) {
        case '\0': escaped[1] = '0'; break;
        case '\n': escaped[1] = 'n'; break;
        case '\r': escaped[1] = 'r'; break;
        case '\t': escaped[1] = 't'; break;
        case '\\': escaped[1] = '\\'; break;
        case '"': escaped[1] = '"'; break;
        default:
            if (byte < 0x20u || byte == 0x7fu) {
                escaped[1] = 'x';
                escaped[2] = hex[byte >> 4u];
                escaped[3] = hex[byte & 0x0fu];
                length = 4u;
            } else {
                escaped[0] = (char)byte;
                length = 1u;
            }
            break;
        }
        if (!append_string(buffer, used, capacity, escaped, length)) return false;
    }
    return append_string(buffer, used, capacity, "\"", 1u);
}

static char *list_to_string(const AphelionList *list) {
    char *buffer = NULL;
    size_t used = 0u, capacity = 0u, i;
    if (list == NULL || !append_string(&buffer, &used, &capacity, "[", 1u)) goto fail;
    for (i = 0u; i < list->length; ++i) {
        if (i != 0u && !append_string(&buffer, &used, &capacity, ", ", 2u)) goto fail;
        if (list->items[i].type == APHELION_VALUE_TEXT) {
            if (!append_quoted_text(&buffer, &used, &capacity, list->items[i].as.text)) goto fail;
        } else {
            char *item = aphelion_value_to_string(list->items[i]);
            bool ok;
            if (item == NULL) goto fail;
            ok = append_string(&buffer, &used, &capacity, item, strlen(item));
            free(item);
            if (!ok) goto fail;
        }
    }
    if (!append_string(&buffer, &used, &capacity, "]", 1u)) goto fail;
    return buffer;
fail:
    free(buffer);
    return NULL;
}

char *aphelion_value_to_string(AphelionValue value) {
    switch (value.type) {
    case APHELION_VALUE_HOLLOW:
        return format_alloc("hollow");
    case APHELION_VALUE_SILENT:
        return format_alloc("silent");
    case APHELION_VALUE_WHOLE:
        return format_alloc("%" PRId64, value.as.whole);
    case APHELION_VALUE_DECIMAL:
        return format_alloc("%.17g", value.as.decimal);
    case APHELION_VALUE_SIGNAL:
        return format_alloc("%s", value.as.signal ? "bright" : "dim");
    case APHELION_VALUE_TEXT:
        if (value.as.text != NULL) {
            size_t i, length = value.as.text->length, used = 0u;
            char *result;
            if (length > (SIZE_MAX - 1u) / 2u) return NULL;
            result = (char *)malloc(length * 2u + 1u);
            if (result == NULL) return NULL;
            for (i = 0u; i < length; ++i) {
                if (value.as.text->bytes[i] == '\0') {
                    result[used++] = '\\'; result[used++] = '0';
                } else result[used++] = value.as.text->bytes[i];
            }
            result[used] = '\0';
            return result;
        }
        return format_alloc("");
    case APHELION_VALUE_GLYPH:
        return format_alloc("U+%04" PRIX32, value.as.glyph);
    case APHELION_VALUE_FUNCTION:
        return format_alloc("<function #%" PRIu32 ">", value.as.function);
    case APHELION_VALUE_LIST:
        return list_to_string(value.as.list);
    default:
        return format_alloc("<invalid>");
    }
}
