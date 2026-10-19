# Aphelion bytecode (0.1)

Aphelion compiles a validated AST to a custom stack instruction stream.  The
VM never evaluates an AST or source string.  A compiled file uses the `.apb`
extension and starts with the four-byte magic `APB1`.

## Instruction record

Each instruction is represented in memory as:

```c
uint8_t  opcode;
int32_t  operand;
int32_t  operand2;
uint32_t line;
uint32_t column;
```

The file format writes those fields explicitly in little-endian order rather
than dumping C structs.  Source locations are retained for VM diagnostics and
disassembly.

## Opcodes

| Opcode | Stack behavior / purpose |
| --- | --- |
| `HALT` | Stop the current execution. |
| `CONSTANT n` | Push constant-pool entry `n`. |
| `HOLLOW`, `SILENT` | Push the corresponding value. |
| `POP`, `DUP` | Discard or copy the top value. |
| `LOAD_LOCAL n`, `STORE_LOCAL n` | Read/write a call-frame slot. |
| `LOAD_GLOBAL n`, `STORE_GLOBAL n` | Read/write a global slot. |
| `MAKE_FUNCTION n` | Push function value for function table entry `n`. |
| `ADD`, `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `REMAINDER` | Numeric/text binary operations. |
| `NEGATE`, `NOT` | Unary operations. |
| `EQUAL`, `NOT_EQUAL`, `LESS`, `LESS_EQUAL`, `GREATER`, `GREATER_EQUAL` | Comparisons. |
| `AND`, `OR` | Non-short-circuit primitive operations; source logical operators compile to jumps. |
| `JUMP n` | Set the current function instruction pointer to `n`. |
| `JUMP_IF_FALSE n`, `JUMP_IF_TRUE n` | Peek at the top value and jump if the condition matches. |
| `CALL argc` | Call the function below `argc` arguments. |
| `RETURN` | Return the top value from the current frame. |
| `REVEAL` | Print one value and push `silent`. |
| `TO_TEXT` | Convert the top value to a new text value. |
| `RECEIVE` | Read one line from VM input and push `text`. |
| `TO_WHOLE`, `TO_DECIMAL` | Perform checked numeric conversion. |
| `LENGTH` | Consume text and push its byte length as whole. |
| `KIND` | Consume an initialized value and push owned type-name text. |
| `ASSERT argc` | Consume one signal and optionally text (`argc` 1 or 2); push silent or fail. |
| `LIST count, tag` | Consume homogeneous primitive values and push an owned immutable list. |
| `INDEX` | Consume list and whole index, check bounds, push a retained element. |
| `SOURCE n` | Select a validated filename text constant for this call frame's diagnostics. |
| `OUTPUT` | Reserved internal opcode; not a source builtin. |
| `BREAK`, `SKIP` | Reserved validation sentinels; the compiler patches source loop control to jumps. |
| `NOP` | No operation. |

The entry function is function zero.  Other functions carry a name, arity,
local-slot count, and instruction stream.  Globals and constants use indexed
tables.  The compiler registers functions and globals before emitting code so
forward function references work.

## File layout

The serialized stream is:

```text
magic[4] = APB1
version:u32
entry_function:u32
constant_count:u32
function_count:u32
global_count:u32
source_name:string
constants[constant_count]
globals[global_count]:string
functions[function_count]:
    name:string
    arity:u32
    local_count:u32
    instruction_count:u32
    instructions[instruction_count]
```

Strings are a u32 byte length followed by bytes, without a serialized NUL.
Values are tagged with an explicit runtime type.  The reader bounds section
counts and string/instruction sizes, validates value tags, checks the entry
function, and rejects truncated files.  The VM additionally checks all index,
stack, call-depth, jump, and opcode operations before executing them.

Whole numbers use explicit two's-complement little-endian encoding; decimal
requires IEEE-754 binary64 and is checked at build time. Text lengths preserve
embedded NUL bytes. Metadata names reject NULs; duplicate global names, nonfinite
decimal constants, invalid glyph scalars, and unused nonzero operands are
rejected. The reader limits files to 64 MiB and verifies strings/instruction
streams fit the remaining bytes before allocating. Opcodes retain their v1
numbers; new conversion/input and builtin instructions were appended, not
inserted. `LENGTH=40`, `KIND=41`, `ASSERT=42`; the v1 file layout is unchanged.
Unused operands must be zero; `ASSERT`'s first operand must be 1 or 2.
The uninitialized slot sentinel is internal and never valid in a constant pool.
`LIST=43` and `INDEX=44` are appended. LIST requires a nonnegative count and
an element tag for whole/decimal/signal/text/glyph; tag HOLLOW is valid only for
count zero. INDEX operands are zero. Lists (runtime tag 9) are constructed by
instructions and are never serialized as recursive constants; list and
uninitialized constant tags are rejected. LENGTH accepts lists as element counts.
SOURCE=45 requires a nonempty NUL-free text constant and zero operand2. Each
frame borrows its filename from bytecode-owned storage; calls default to the
entry source and returns restore the caller's marker. Linked modules emit these
markers; single-file programs keep their existing instruction stream. `.apb`
execution does not perform source/module I/O.

## Runtime ownership

The constant pool owns one reference to each text constant.  `LOAD_*`, `DUP`,
and constant pushes retain text values; stores and pops transfer ownership.
Call frames own their local slots.  Returning destroys locals and transfers the
return value to the caller.  The VM destroys all remaining values on success or
controlled failure. Function values are indexes, not pointers. Lists own retained
primitive elements and their own reference count; copy/return/indexing retain
payloads, and final list destruction releases each element. Nested/mutable
collections are not part of this first representation, so cycles cannot arise.
