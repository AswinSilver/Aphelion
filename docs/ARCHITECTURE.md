# Compiler architecture and ownership

Aphelion is split into independent C modules.  Public interfaces live under
`include/aphelion/` and implementations live under `src/`.

| Module | Responsibility |
| --- | --- |
| `common.h` / diagnostics | Source locations and owned diagnostic messages. |
| `token.h` / `token.c` | Token kinds, keyword lookup, token names. |
| `lexer.h` / `lexer.c` | UTF-8 byte scanning, comments, literals, operators, locations. |
| `ast.h` / `ast.c` | Explicit expression, statement, block, function, and program nodes. |
| `parser.h` / `parser.c` | One-token-lookahead recursive descent and expression precedence. |
| `module.h` / `module.c` | Bounded canonical module graph, independent parsing and AST linking. |
| `file.h` / `file.c` | Checked UTF-8-to-wide Windows file opens/stat, native-byte POSIX file operations. |
| `semantic.h` / `semantic.c` | Nested scopes, symbols, type inference, mutability, signatures, control-flow checks. |
| `bytecode.h` / `bytecode.c` | Instruction vectors, constants, functions, `.apb` serialization, disassembly. |
| `value.h` / `value.c` | Tagged runtime values, text/list reference counting, equality and output formatting. |
| `compiler.h` / `compiler.c` | Validated AST to stack bytecode, locals, globals, jumps, calls. |
| `vm.h` / `vm.c` | Bounded operand stack, call frames, instruction dispatch, runtime errors. |
| `main.c` | Native argv normalization, file loading, CLI/debug routing, diagnostics, and bounded REPL. |

## Front-end flow

`main.c` owns the loaded source buffer.  The lexer borrows that NUL-terminated
buffer and emits token spans; diagnostics copy their messages.  The parser
copies identifiers and decoded string contents into the AST.  Once parsing is
complete, the parser destroys its lexer diagnostics only after copying them to
the caller's `Diagnostics` collection.

The semantic pass first predeclares global functions and declarations, then
checks initializers and function bodies.  Expression nodes receive an
`inferred_type` and a borrowed `resolved_symbol` pointer for compiler clients.
Semantic errors prevent bytecode generation.

## Compiler and VM flow

The compiler creates an entry function, registers named functions and globals,
collects local slots, and emits instructions.  Function calls use a stack
layout of callee followed by arguments; the VM replaces that area with the
callee's local frame.  Returning destroys local values and leaves one result on
the caller's operand stack.

Conditional jumps peek at their condition.  The compiler emits explicit pops
on each branch.  `and` and `or` short-circuit by jumping around the RHS.  Loop
`escape` and `skip` are compiled into patched jumps, so the VM does not need to
interpret source-level control keywords.

## Memory rules

- The lexer borrows caller source/filename. The parser copies the filename into
  its AST; spans/diagnostics borrow that owned name and must be inspected before
  freeing the AST. Initial allocation-failure diagnostics can borrow the caller
  filename. Module linking retains/transfers filename ownership in the root's
  source_files table through diagnostic printing and AST destruction.
- `Diagnostics` owns copied message strings and releases them on clear/free.
- The Windows CLI owns an all-or-nothing UTF-8 copy of native wide arguments
  until routing finishes. File helpers free temporary wide paths/modes before
  returning, preserve errno on failure, and never publish partial conversions.
- AST constructors own copied names/text; `ast_program_free` recursively frees
  every child.
- The parser also uses public subtree destructors for allocations that cannot
  be attached to a parent. Recursion and AST depth limits bound later recursive
  analysis, compilation, printing, and cleanup.
- Bytecode owns function/global names, instruction arrays, constants, and its
  source name.
- `AphelionValue` text payloads are reference counted.  A value copy retains;
  a move transfers; destroy releases.
- Immutable lists own retained primitive elements. They cannot contain nested
  collections/cycles. Checked indexing retains an element; rebinding releases
  only the binding's old reference. Traversal holds one immutable snapshot and
  explicitly clears its hidden/list/element slots at loop exit.
- Text payloads carry a byte length, including embedded NULs. `to_text(text)`
  moves the retained value directly; debug rendering escapes NULs. AST-to-VM
  compilation and `.apb` I/O use lengths rather than `strlen` for text values.
- The VM owns its stack, frames, and global value array and clears them on both
  success and controlled failure.
- The VM borrows `stdin` (or a caller-provided input stream) for `RECEIVE` and
  never closes that stream.  Input lines are accumulated into owned text before
  the value is pushed.
- Operand pops cannot consume frame-local slots. The VM validates in-memory
  bytecode, checks slot initialization, and has an adjustable instruction budget.
  Allocation-failure injection in `tests/support/` exercises partial cleanup
  without replacing the production allocator or adding a runtime dependency.

No module calls `system`, `popen`, PowerShell, or another shell.  File loading
is limited and uses binary reads with size/error checks.

Source modules are canonical-root checked with guarded native Windows/POSIX
filesystem APIs, independently parsed and statically linked in dependency order.
SOURCE instructions retain per-frame diagnostic filenames in serialized programs;
VM execution never concatenates/parses sources or repeats module loading.

## Current boundary

Function-local slots are allocated in one frame for efficient VM access, but
each compiler slot retains the AST declaration/parameter identity resolved by
semantic analysis.  Nested blocks can therefore shadow a name without changing
which binding later reads or writes.  Block lifetime is lexical at compile
time; the VM frame retains the slots for the duration of the function call.
