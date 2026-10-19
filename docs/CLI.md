# Aphelion CLI (0.1)

The executable is named `aphelion.exe` on Windows and `aphelion` on Unix-like
systems.

## Commands

```text
aphelion run file.agne
```

Reads a `.agne` file, lexes and parses it, performs semantic analysis, compiles
it to in-memory bytecode, and executes the entry function.  Program output is
written to stdout; `receive()` reads stdin; diagnostics are written to stderr.

```text
aphelion check file.agne
```

Runs the front end and compiler but does not execute the VM.  A successful
check prints `file.agne: valid`.

```text
aphelion compile file.agne
```

Runs the same validation and writes `file.apb` beside the source.  It prints
the output path only after the file is successfully closed.

```text
aphelion tokens file.agne
aphelion ast file.agne
```

These debugging commands print lexer tokens or a readable AST.  `tokens`
returns a nonzero status when lexical errors are encountered; `ast` performs
syntax parsing but intentionally does not run semantic analysis.

```text
aphelion disasm file.apb
```

Validates and reads a portable bytecode file, then prints its source name,
constant pool, globals, function metadata, source locations, and instructions.

```text
aphelion --help
aphelion --version
```

```text
aphelion repl
```

Runs a one-line, fresh-scope compiler/VM REPL. Expressions are echoed; silent
results are not echoed twice. Declarations do not persist between lines; use
semicolons for related statements on one line. `:help`, `:quit`, and `:q` are
supported. EOF exits cleanly. Incomplete syntax is diagnosed, not buffered as
multiline input. Each line is limited to 64 KiB; failures recover but make the
final session status 1. Prompts are terminal-only. `summon` is file-only in this
first REPL. See `REPL.md` for complete semantics and examples.

Source commands reject paths without the `.agne` suffix, and
`disasm` rejects paths without `.apb`.

Windows process arguments are decoded from native UTF-16 to UTF-8; invalid
UTF-16 is rejected. Source and bytecode file operations use wide filesystem
APIs, so filenames and module-root directories can contain Unicode and spaces
without depending on the active ANSI code page. POSIX filenames remain native
byte strings. This does not relax the ASCII path-segment rules inside `summon`
strings or add Unicode identifiers/multibyte glyph decoding to the language.

## Exit statuses

| Status | Meaning |
| ---: | --- |
| 0 | The requested command completed successfully. |
| 1 | Source, bytecode, semantic, compilation, runtime, or REPL input/evaluation failure. |
| 2 | Invalid command-line usage or a reserved unsupported command. |

Diagnostics have the form:

```text
path/to/file.agne:line:column: message
```

The CLI limits source files to 64 MiB.  It does not execute shell commands
from source or invoke a host interpreter.
