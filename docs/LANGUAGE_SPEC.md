# Aphelion language specification (0.1)

This document describes the language that is implemented by this repository.
Words in the **planned** section are not accepted by the current compiler.

## Source model

Aphelion source is UTF-8 text in a file ending in `.agne`.  Identifiers are
ASCII and case-sensitive.  An identifier starts with a letter or `_` and may
then contain letters, digits, or `_`.  Keywords cannot be used as identifiers.

Whitespace is ignored except that a line break can terminate a statement.
Statements may also use `;`.  A line break never terminates a string or an
expression while the parser is waiting for an operator, closing delimiter, or
other continuation token.  Braces delimit blocks.

Comments are:

```agne
// from here to the end of the line
/* from here, possibly across lines, to the next */
```

Unterminated comments, strings, and glyphs are lexical errors.

## Vocabulary

| Word | Meaning |
| --- | --- |
| `forge` | Immutable variable declaration. |
| `shift` | Mutable variable declaration. |
| `anchor` | Immutable declaration whose initializer must be compile-time evaluable. |
| `orbit` | Function declaration. |
| `yield` | Return from the current function. |
| `when` | Conditional statement. |
| `otherwise` | Alternative branch. |
| `cycle` | While loop. |
| `escape` | Exit the nearest `cycle`. |
| `skip` | Continue the nearest `cycle`. |
| `traverse` | Visit a typed immutable list with a loop-local element binding. |
| `summon` | Statically load a root-relative source module. |
| `reveal` | Built-in that writes one value and a newline. |
| `receive` | Built-in that reads one input line as text. |
| `to_text` | Built-in that converts a runtime value to text. |
| `to_whole` | Checked conversion to a signed 64-bit integer. |
| `to_decimal` | Checked conversion to a finite decimal value. |
| `length` | Byte length of text, returned as `whole`. |
| `kind` | Stable runtime type-name text. |
| `assert` | Check a signal, optionally with a text failure message. |
| `bright` / `dim` | Boolean literals. |
| `hollow` | Null-like literal. |
| `whole` | Signed 64-bit integer type. |
| `decimal` | IEEE-754 double type. |
| `signal` | Boolean type. |
| `text` | Length-delimited byte string with a convenience trailing NUL. |
| `glyph` | A decoded character/code point value. |
| `silent` | No-return-value function type. |

The compiler does not accept conventional aliases such as `let`, `fn`, `if`, `else`, `while`,
`return`, `true`, or `false`.

## Bindings and scopes

Declarations require an initializer:

```agne
forge inferred = 20
forge price: decimal = 19.95
shift active: signal = bright
anchor LIMIT = 100
```

The annotation can also appear before the name (`forge whole count = 0`) for
compatibility with the initial grammar.  Inference chooses the initializer's
type.  `forge` and `anchor` cannot be assigned after declaration; `shift` can
be assigned, but an assignment must preserve the declared type.  `whole` may
widen to `decimal` where a decimal value is expected; no other implicit
conversion is performed.

Blocks create nested semantic scopes.  Names must be unique within one scope,
and an inner scope can resolve a name from its parent.  Functions have their
own parameter/local scope and can resolve global declarations and functions.
Top-level statements and initializers are checked/executed in source order.
A function called before a referenced global is initialized fails at runtime;
`hollow` is distinct from an uninitialized slot. Conversion and
`length`/`kind`/`assert` builtin names may not be redeclared, including parameters
and inner scopes. `silent` is not a variable or parameter value type.

An `anchor` expression may contain literals, previously defined anchors,
grouping, unary operators, and binary operators.  Calls and mutable variables
are not compile-time constants.

## Functions

Functions require typed parameters and an explicit return type:

```agne
orbit add(left: whole, right: whole) -> whole {
    yield left + right
}

orbit greet(name: text) -> silent {
    reveal("Hello " + name)
    yield
}
```

Non-`silent` functions must yield a compatible value on every reachable path.
A `silent` function may use bare `yield` and cannot yield a value.  Calls are
statically checked for arity and argument types; recursion is supported.

## Expressions

The implemented operators, from highest to lowest precedence, are:

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 | calls, indexing, and grouping | left |
| 2 | unary `-`, `+`, `!`, `not` | right |
| 3 | `*`, `/`, `%` | left |
| 4 | `+`, `-` | left |
| 5 | `<`, `<=`, `>`, `>=` | left |
| 6 | `==`, `!=` | left |
| 7 | `and`, `&&` | left |
| 8 | `or`, `||` | left |
| 9 | `=`, `+=`, `-=`, `*=`, `/=` | right |

`+` adds numeric values or concatenates two `text` values.  Other arithmetic
operators require numbers; `%` requires two `whole` values.  Numeric results
are `decimal` when either operand is decimal and otherwise `whole`.  Integer
overflow is a controlled runtime error.  Division by zero and remainder by
zero are runtime errors.  `and` and `or` are statically signal-only and are
compiled with short-circuit jumps.

Whole division truncates toward zero; remainder has the dividend's sign.
Negating the minimum whole or dividing/remaindering it by -1 traps. The minimum
literal `-9223372036854775808` is valid; its positive magnitude is not. Overflow
checks use integer bounds and do not depend on host floating-point precision.
Whole-to-decimal widening is performed at storage, argument, and return sites.

Comparisons produce `signal`.  Ordered comparisons support numeric values,
`text` values (lexicographically by their length-delimited bytes), and `glyph`
values. Equality supports equal-compatible primitive values. Conditions
of `when` and `cycle` must have type `signal`; Aphelion does not use implicit
truthiness in source-level conditions.
Whole/whole equality and ordering are exact. Mixed whole/decimal comparisons
convert the whole to binary64 first; precision above 2^53 may be lost.

## Runtime output

The essential builtins have these exact signatures:

| Signature | Accepted arguments and result |
| --- | --- |
| `reveal(value) -> silent` | Exactly one runtime value; writes its stable representation and one newline. |
| `receive() -> text` | No arguments; reads one line from standard input, excluding its line ending. |
| `to_text(value) -> text` | Exactly one runtime value; text is an identity conversion, other values use their stable representation. |
| `to_whole(value) -> whole` | Exactly one `whole`, finite `decimal`, or numeric `text`; truncates decimals toward zero. |
| `to_decimal(value) -> decimal` | Exactly one `whole`, finite `decimal`, or `strtod`-grammar numeric `text`. |
| `length(text-or-list) -> whole` | Exactly one `text` or primitive list; counts bytes or elements. |
| `kind(value) -> text` | Exactly one initialized runtime value; returns a stable type name. |
| `assert(signal[, text]) -> silent` | One signal and optional text message; false signals are runtime failures. |

`reveal` does not support multiple arguments; concatenate text explicitly when
one line needs several pieces. Wrong arity or static argument types are
location-bearing semantic errors, before bytecode is written. Runtime input,
conversion, assertion, allocation, and output failures return status 1.

`reveal(value)` writes one line. Stable representations are:

| Value | Output |
| --- | --- |
| `bright`, `dim` | `bright`, `dim` |
| `hollow` | `hollow` |
| `silent` | `silent` |
| text | its UTF-8 bytes |
| glyph | `U+XXXX` |
| function | `<function #N>` |

`receive()` reads from standard input until a newline or EOF.  The returned
`text` excludes `\n` and a paired `\r`; an empty line and EOF before any bytes
both return empty text.  Input errors are runtime failures.  Lines are grown
dynamically, so the VM does not silently truncate long input. The builtin accepts
no arguments and works in source programs, functions, and redirected or
interactive CLI execution. Input bytes, including embedded NUL bytes, are
retained.

The conversion builtins have these rules:

- `to_text(value)` accepts every runtime value and returns its stable printable
  representation. For text this is identity and preserves embedded NUL bytes.
- `to_whole(value)` accepts `whole` (identity), finite `decimal` (truncated
  toward zero when the result fits `int64_t`), and `text` containing only an
  optional sign followed by decimal digits.  Invalid text, non-finite values,
  and overflow are runtime errors.
- `to_decimal(value)` accepts `whole`, finite `decimal`, and `text` accepted by
  the C `strtod` grammar with no surrounding whitespace.  Invalid text,
  non-finite values, and range errors are runtime errors.

Numeric-to-text formatting uses the stable decimal/whole representations shown
by `reveal`; text conversion preserves the original length-delimited bytes,
including embedded NULs.

The inspection/assertion builtins use ordinary eager, left-to-right argument
evaluation and real VM instructions:

- `length(text-or-list) -> whole` takes exactly one text/list argument. Text counts **bytes**,
  not Unicode characters/code points: `length("é")` is 2 and `length("a\0b")`
  is 3. Empty text has length zero; a list returns its element count.
  Other types/arity are static errors.
- `kind(value) -> text` takes exactly one argument. Stable names are `whole`,
  `decimal`, `signal`, `text`, `glyph`, `hollow`, `silent`, and `list`. For example,
  `kind(hollow)` is `"hollow"`; `kind(reveal(1))` prints 1 then returns
  `"silent"`. VM function values use `"function"`, although bare function
  values are not source-level first-class values. Uninitialized slots cannot
  be observed: loading one fails before `kind` executes.
- `assert(condition[, message]) -> silent` requires one signal condition and,
  if present, a text message. `bright` succeeds without output; `dim` produces
  a source-located `assertion failed` runtime diagnostic and exit status 1.
  Messages are evaluated even on success. Diagnostics escape NUL/control bytes
  and bound long messages; the original text value is not truncated or changed.
  Wrong arity/types are static errors, with defensive checks in the VM too.

See `../examples/builtins.agne` for an executable example.

## Immutable primitive lists

```agne
forge numbers = [1, 2, 3]
forge empty: [text] = []
orbit first(values: [whole]) -> whole { yield values[0] }
shift words: [text] = ["old", "a\0b"]
forge retained = words
words = ["new"]
assert(length(retained) == 2)
reveal(first(numbers))
```

One list type supports exact homogeneous `whole`, `decimal`, `signal`, `text`,
or `glyph` elements. Its annotation is `[element-type]`. Mixed element types,
nested lists, hollow/silent/function elements, and whole-list numeric widening
are rejected. Empty bindings require an annotation; empty literals are also
valid typed arguments/returns/assignments and operands to `length`/`kind`.
Indexing an untyped `[]` is a static error because no element type is known.

Indexes are zero-based whole values. Negative/out-of-range indexing of a typed
list fails at runtime, including empty lists; other index types/non-list targets
fail statically. Lists are immutable: indexed assignment and structural
mutation are not supported. A `shift` binding may be rebound to a compatible
list without changing retained aliases. Types are invariant. Equality compares
ordered elements and length; an empty literal compares as an empty sequence.
Arithmetic and ordered comparisons on lists are invalid.

`length(list)` counts elements, `kind(list)` is `"list"`, and `reveal`/`to_text`
use bracketed comma-separated output with quoted, escaped text elements.
Reference-counted list storage retains element values through function returns,
rebindings, and nested scopes and releases them on normal/error cleanup.
See `COLLECTIONS_DESIGN.md` and `../examples/collections.agne`.

## Traversal

```agne
forge values = [1, 2, 3]
traverse item in values {
    when item == 2 { skip }
    reveal(item)
}
```

`traverse IDENTIFIER in expression { ... }` evaluates its typed-list iterable
exactly once, before declaring the iteration binding. It visits elements in
index order and retains that immutable snapshot even if its source binding is
rebound in the body. The loop binding is immutable, has the list's element
type, shadows an outer name, and is not visible after the loop. Ordinary inner
body scopes may shadow it. There is no index binding in this first syntax.

Empty typed lists run zero iterations. An untyped `[]` iterable needs a typed
binding/function result first so the element type is known. Text and non-list
iterables are rejected. Nested `cycle`/`traverse` loops use the nearest loop
for `escape` (exit) and `skip` (advance, not retest the same element).
Functions still require a guaranteed yield outside possibly-empty loops.

Traversal compiles to bounded VM operations, not a host loop or AST interpreter.
Loop-private list/index/element slots are cleaned up on exit, and function
return/error cleanup releases them too. The existing instruction budget bounds
work inside a loop; an infinite body is not exempt from that budget.

## Source modules

`summon "lib/math"` in an entry file's initial top-level preamble loads
`lib/math.agne` under the entry file's canonical directory. Nested modules use
that same root, not their own directory. Names consist only of slash-separated
ASCII identifier segments; omit `.agne`. Absolute/drive/UNC paths, backslashes,
empty/dot/dotdot segments, NUL, and canonical symlink/junction escapes are
rejected. Imports cannot occur after declarations/work or inside blocks.

Each canonical path initializes once per program, in deterministic dependency-
first/source-import order. Visiting-stack cycles are errors. All top-level
declarations are exported into one shared namespace, including transitive
imports; collisions are errors. There are no aliases/private/module-member
names yet. Distinct canonical hard-link paths are distinct module identities.
Function linking precedes initialization; uninitialized global reads still fail.

Each file is independently parsed and its AST linked, not textually included.
Limits including the entry are 256 files, 64 active modules, and 64 MiB aggregate
source. Only regular/seekable files load. Compiled `.apb` files are self-contained
and preserve per-frame source filenames without loading modules at execution.
See `MODULES_DESIGN.md` and `../examples/modules.agne`.

## Implemented limitations

The `aphelion repl` command accepts one physical line as an independent program
with fresh scope, echoes direct top-level non-silent expression values, and
supports `:help`/`:quit`/`:q` and EOF. Declarations do not persist. Related
statements fit on one line with semicolons/braces; incomplete input is diagnosed
rather than buffered as multiline input. Lines are bounded to 64 KiB, and errors
recover for the next line while retaining final failure status. Imports remain
file-only in this REPL. See `REPL.md` for exact input/terminal behavior.

There are no nested/mutable collections, member access, records, closures,
default parameters, user-defined generic types, or persistent/multiline REPL
state. Module root protection is not an OS sandbox against concurrent
filesystem races.

Raw NUL source bytes are rejected, but `\0` and `\x00` in text literals are
preserved through concatenation, conversion, and serialization. Full UTF-8
validation and multibyte glyph decoding remain deferred (glyph literals support
single bytes/escapes currently). Source strings cannot span physical lines.
Parser recursion and expression-tree depth are each limited to 128; VM operand
slots to 8192, call frames to 256, and executed instructions to 10,000,000 by
default. Embedders can adjust the VM limits through its C API. Limits yield
controlled errors, not host stack exhaustion. See `RELIABILITY.md` for test scope.
