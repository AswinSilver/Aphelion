#ifndef APHELION_VALUE_H
#define APHELION_VALUE_H

/*
 * Runtime values used by the bytecode interpreter.
 *
 * Text and list values are reference counted.  A value returned by one of the
 * aphelion_value_* constructors owns its reference; copying a value must be
 * done with aphelion_value_copy(), and every owned value must eventually be
 * passed to aphelion_value_destroy().  The bytecode constant pool and the VM
 * both follow those rules.  The character buffer in AphelionText is UTF-8
 * data, but the VM deliberately does not impose a Unicode implementation on
 * glyph values (a glyph is a Unicode scalar/code point held in uint32_t).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AphelionText AphelionText;
typedef struct AphelionList AphelionList;

typedef enum AphelionValueType {
    APHELION_VALUE_HOLLOW = 0,
    APHELION_VALUE_WHOLE,
    APHELION_VALUE_DECIMAL,
    APHELION_VALUE_SIGNAL,
    APHELION_VALUE_TEXT,
    APHELION_VALUE_GLYPH,
    APHELION_VALUE_FUNCTION,
    APHELION_VALUE_SILENT,
    APHELION_VALUE_UNINITIALIZED, /* VM-only sentinel, never serialized */
    APHELION_VALUE_LIST
} AphelionValueType;

typedef struct AphelionValue {
    AphelionValueType type;
    union {
        int64_t whole;
        double decimal;
        bool signal;
        uint32_t glyph;
        AphelionText *text;
        AphelionList *list;
        uint32_t function;
    } as;
} AphelionValue;

struct AphelionText {
    size_t references;
    size_t length;
    char bytes[];
};

/* Immutable homogeneous primitive elements, each owned by this storage. */
struct AphelionList {
    size_t references;
    size_t length;
    AphelionValueType element_type;
    AphelionValue items[];
};

AphelionValue aphelion_value_hollow(void);
AphelionValue aphelion_value_silent(void);
AphelionValue aphelion_value_whole(int64_t value);
AphelionValue aphelion_value_decimal(double value);
AphelionValue aphelion_value_signal(bool value);
AphelionValue aphelion_value_glyph(uint32_t value);
AphelionValue aphelion_value_function(uint32_t function_index);
AphelionValue aphelion_value_text(const char *text);
AphelionValue aphelion_value_text_n(const char *text, size_t length);
/* Retain a copy of every item; return hollow on invalid input or allocation
 * failure. Empty lists accept a primitive element type or hollow. */
AphelionValue aphelion_value_list(AphelionValueType element_type,
                                  const AphelionValue *items, size_t length);

/* Return a retained copy.  It is valid for value and result to alias. */
AphelionValue aphelion_value_copy(AphelionValue value);

/* Transfer value into result and turn value into hollow. */
AphelionValue aphelion_value_move(AphelionValue *value);

/* Release owned payloads and reset the value to hollow. */
void aphelion_value_destroy(AphelionValue *value);

const char *aphelion_value_type_name(AphelionValueType type);
bool aphelion_value_truthy(AphelionValue value);
bool aphelion_value_equal(AphelionValue left, AphelionValue right);

/* Print a stable, human-readable representation.  Returns false on I/O error. */
bool aphelion_value_write(FILE *out, AphelionValue value);

/* Allocate a newly-owned printable representation.  Caller frees it. */
char *aphelion_value_to_string(AphelionValue value);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_VALUE_H */
