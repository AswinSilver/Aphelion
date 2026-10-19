# Minimal source modules

This contract was proposed before implementation and is now implemented; actual
Windows/Linux/sanitizer results are in `../DEVELOPMENT_STATUS.md`. It uses independently parsed ASTs and the existing
semantic/compiler/bytecode VM, never source concatenation or regex rewriting.

## Syntax, roots, and identity

`summon "math/util"` loads `math/util.agne` under the canonical directory of
the entry `.agne` file. All imports use that one explicit root, including
imports inside subdirectories; there is no CWD/environment/package search.
Names are slash-separated ASCII identifier segments, without `.agne` suffix.
Reject empty segments, `.`, `..`, backslashes, drive/absolute/UNC paths, NUL,
and other characters. Resolve actual symlink/junction targets and enforce the
canonical root boundary, not merely a textual path prefix.

Imports are top-level preamble statements before declarations/other work.
Nested/conditional/function imports are invalid. Module identity is canonical
path (case-insensitive on Windows); repeated/diamond imports initialize once
per program. Distinct canonical paths, including hard-link names, are distinct
modules. Detect active-stack cycles with useful importing-file diagnostics.

## Names and initialization

This first design is an open static import: every top-level declaration is
exported into the linked program's shared global namespace, including transitive
imports. There are no aliases/private exports/namespaced members yet. Duplicate
declarations across modules or the entry file are semantic errors, not last-wins.

Dependencies initialize before their importer, once in deterministic import
source order; each module's own statements retain source order. Function
declarations are linked before initialization as in existing programs. Early
global reads still fail rather than producing hollow. A compiled `.apb` is
self-contained and never reloads sources during VM execution.

## Ownership, diagnostics, and portability

The loader retains source filenames for AST spans/diagnostics and transfers AST
items, not source bytes, to the linked program. Partial graphs/ASTs/files must
unwind on errors/OOM. Limits including the entry are 256 modules, 64 active
modules, and 64 MiB aggregate source. Reject non-regular/unseekable files.

Use guarded Windows APIs for canonical junction paths and POSIX canonical paths
on Linux/macOS; no unconditional POSIX dependency on Windows. This is accidental
traversal protection, not an OS sandbox against concurrent filesystem races.

Append a SOURCE instruction referencing a validated non-NUL text constant for
module filenames. Each call frame retains its source marker so nested calls
and returns restore the correct diagnostic file. Retain v1 encoding and old
opcode numbers; programs without imports should not need extra markers.

Acceptance: nested/diamond/duplicate imports, missing/malformed files, cycles,
name collisions, initialization order, traversal and symlink/junction escapes,
source-located runtime failures, standalone serialized execution, OOM cleanup,
strict Windows/Linux builds/full suites/sanitizers/fuzzing, with macOS/remote
status distinguished explicitly.
