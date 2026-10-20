# Aphelion

Aphelion is a small, statically typed programming language implemented from
scratch in C11.  Aphelion source files use the `.agne` extension and are
compiled to a custom stack-based bytecode before they execute in the Aphelion
virtual machine.

The project is intentionally a real compiler pipeline, not a wrapper around
another language:

```text
.agne source -> lexer -> AST parser -> semantic checker -> bytecode compiler
             -> .apb bytecode / stack VM
```

## Current release

The checked-in release identity is `include/aphelion/version.h`. Version
`0.1.0` implements the following core. Start with
[`docs/QUICKSTART.md`](docs/QUICKSTART.md) for platform-specific setup and
[`docs/RELIABILITY.md`](docs/RELIABILITY.md) for this stage's safety contract.

- UTF-8 source input with line and column diagnostics.
- A hand-written C lexer with single-line and multiline comments.
- A hand-written recursive-descent parser and explicit AST.
- Inferred and explicitly annotated primitive value types: `whole`, `decimal`,
  `signal`, `text`, `glyph`; `silent` is a function-return type only.
- Immutable `forge` bindings, mutable `shift` bindings, and constant-expression
  `anchor` bindings (structurally checked; compile-time folding is deferred).
- `orbit` functions with typed parameters, typed returns, recursion, local
  variables, and `yield`.
- `when`/`otherwise`, `cycle`, `escape`, and `skip` control flow.
- Arithmetic, comparisons, short-circuit `and`/`or`, unary `not`, assignment,
  compound assignment, calls, strings, glyphs, `bright`, `dim`, and `hollow`.
- Numeric, lexicographic byte-oriented text, and glyph ordering comparisons;
  equality remains length-aware for embedded-NUL text.
- The `reveal(value)` output builtin.
- The `receive()` input builtin, which reads one line from standard input and
  returns text without its line ending.  EOF produces empty text.
- The `to_text(value)` conversion builtin.
- `to_whole(value)` and `to_decimal(value)` for checked numeric/text
  conversions.
- `length(text)` for byte lengths, `kind(value)` for stable type names, and
  `assert(signal, message?)` for checked runtime assertions.
- Immutable homogeneous primitive lists with `[whole]`-style types, checked
  indexing, element-count `length`, sequence equality, and owned text elements.
- `traverse item in list` iteration with loop-local immutable bindings, snapshot
  semantics, nested loops, `escape`, and advancing `skip`.
- Root-relative `summon "lib/math"` source modules with checked canonical paths,
  cycle detection, once-only initialization, and self-contained bytecode.
- A real one-line, fresh-scope `repl` with compiler-driven expression echo,
  bounded input, error recovery, terminal prompts, and clean EOF/quit behavior.
- A practical builtin example combining `receive`, checked conversions,
  `reveal`, `length`, `kind`, `assert`, functions, conditions, and a loop.
- Custom bytecode with a portable little-endian `.apb` file format.
- A bounded stack VM with tagged runtime values and reference-counted text.
- Bounded parser recursion, exact whole arithmetic/comparisons, actual numeric
  widening, and controlled failures for uninitialized values and VM exhaustion.
- CLI commands for running, checking, compiling, token inspection, AST
  inspection, disassembly, help, and version reporting.

Nested/mutable collections, persistent/multiline REPL state, records, and a garbage collector
are deliberately not part of this release.
They are listed as future work rather than being accepted as fake syntax.

## Building

### GCC / MinGW

Builds need a C11 compiler and its platform SDK/C runtime, not another language
runtime. From the project root in Git Bash/MSYS2 or a POSIX shell:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
```

PowerShell should expand the source list explicitly:

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude (Get-ChildItem src/*.c).FullName -lm -o aphelion.exe
```

On Linux or macOS the same command can produce `aphelion` instead of
`aphelion.exe`. Native Windows builds use guarded Windows APIs for UTF-8
filenames. Before distributing a Windows binary, inspect and supply any
toolchain runtime DLLs as described in `docs/RELEASE_GATE.md`; native does not
mean standalone.

The repository also contains a GNU-make `Makefile` (use `gmake` on macOS,
where the system `make` is BSD make) and a
`CMakeLists.txt` for environments with CMake.  CMake is configured for C11 and
does not add third-party dependencies.

On Linux, use `make` for the commands below. On macOS, install GNU Make with
`brew install make` and use `gmake` instead. On Windows, follow the native
MinGW/UCRT64 instructions in `docs/RELEASE_GATE.md` and use `mingw32-make`.

```sh
make build
make test
make sanitize   # requires ASan/UBSan runtime support
make clean
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

## Running a program

```powershell
.\aphelion.exe run examples/factorial.agne
```

For a complete input/conversion example:

```powershell
"Ada`n7`n2.5`n" | .\aphelion.exe run examples/builtins_practical.agne
```

It prints `Hello Ada`, `3`, `whole`, `7 items`, `5`, and `7` on separate lines.

The source command is deliberately explicit:

```text
aphelion run file.agne       compile and execute source
aphelion check file.agne     parse and type-check without executing
aphelion compile file.agne   write file.apb beside the source
aphelion tokens file.agne    print lexer tokens and locations
aphelion ast file.agne       print the parsed AST
aphelion disasm file.apb     print bytecode instructions
aphelion repl                one-line, fresh-scope interactive compiler/VM
aphelion --version           print the compiler version
aphelion --help              print command help
```

`compile` replaces the final `.agne` suffix with `.apb`.  The bytecode reader
checks the magic identifier, version, section counts, value encodings, and
entry function before accepting a file.

## A first program

```agne
anchor LIMIT = 3
shift count: whole = 0

orbit twice(value: whole) -> whole {
    yield value * 2
}

cycle count < LIMIT {
    reveal(twice(count))
    count += 1
}
```

`when` and `cycle` accept either the example-style condition or a parenthesized
condition.  Both of these are valid:

```agne
when count >= 2 { reveal("ready") }
when (count >= 2) { reveal("ready") }
```

Statements may end with a semicolon or a newline.  A newline is not emitted
inside a string and does not split an incomplete expression.

## Diagnostics and safety

Diagnostics use `file:line:column: message` and are produced for lexical,
syntax, semantic, bytecode, and runtime failures.  The VM rejects stack and
call-depth exhaustion, division by zero, invalid operands, invalid jumps,
unknown opcodes, and malformed calls.  Aphelion source is never passed to
`system`, PowerShell, or another host shell.

Text objects are reference counted.  AST strings, bytecode strings, VM values,
and compiler allocations have separate ownership rules documented in their
headers.  The implementation uses bounded file loading and checks allocation
and arithmetic sizes before growing arrays.

## Testing

The integration suite is written with pytest.  It builds all `src/*.c` files
with warnings as errors and tests the public CLI, lexer output, parsing and
type checking, control flow, recursion, strings, runtime errors, bytecode
serialization, and disassembly:

```powershell
python -m pip install pytest
python -m pytest -q
```

The test suite requires Python 3.10+ and a `gcc` executable on `PATH`.  The direct
GCC command above is the authoritative build check when pytest is unavailable.
`CC` and `APHELION_TEST_CFLAGS` configure test-driver builds;
`APHELION_TEST_EXECUTABLE` tests a specified prebuilt binary. Tests include
deterministic source/bytecode fuzzing, injected allocation failures across the C
pipeline/loader, hostile VM streams, and bytecode read/write/execute round-trips.
CI configures strict Linux, Windows MinGW, and macOS Clang jobs plus Linux
sanitizers; remote CI success is not implied by the presence of the workflow.

```sh
python tools/fuzz_harness.py source --executable ./aphelion --cases 500 --seed 2026
python tools/fuzz_harness.py bytecode --executable ./aphelion --cases 500 --seed 2026
```

Fuzzing checks bounded finite corpora, not exhaustive safety. Sanitizer test
commands and the local PIE startup workaround are in `docs/QUICKSTART.md`.

## Documentation

- [`docs/LANGUAGE_SPEC.md`](docs/LANGUAGE_SPEC.md): implemented semantics and
  vocabulary.
- [`docs/GRAMMAR.md`](docs/GRAMMAR.md): lexical and syntactic grammar.
- [`docs/BYTECODE.md`](docs/BYTECODE.md): instruction set and `.apb` layout.
- [`docs/CLI.md`](docs/CLI.md): command behavior and exit statuses.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): module boundaries and
  ownership rules.
- [`docs/QUICKSTART.md`](docs/QUICKSTART.md): install, build, run, and troubleshoot.
- [`docs/RELIABILITY.md`](docs/RELIABILITY.md): semantics, limits, and safety scope.
- [`docs/RELEASE_GATE.md`](docs/RELEASE_GATE.md): native builds and actual CI run instructions.
- [`docs/RELEASE.md`](docs/RELEASE.md): reviewed Windows/Linux binary packages,
  installation, runtime dependencies, and the manual packaging workflow.
- [`docs/COLLECTIONS_DESIGN.md`](docs/COLLECTIONS_DESIGN.md): immutable list types and ownership.
- [`docs/MODULES_DESIGN.md`](docs/MODULES_DESIGN.md): canonical module loading and linking.
- [`docs/REPL.md`](docs/REPL.md): interactive behavior, examples, and state/input limits.
- [`DEVELOPMENT_STATUS.md`](DEVELOPMENT_STATUS.md): verified checks, design
  decisions, and the next implementation boundary.

## License

Aphelion is distributed under the MIT License. See [`LICENSE`](LICENSE) for the
complete license text.

## Roadmap

The next safe increments are persistent/multiline REPL design, richer conversion
and collection APIs, broader platform/fuzz coverage, and eventually a tracing or
generational garbage collector if the runtime grows beyond reference-counted
text and acyclic primitive lists. Each feature should be added with lexer,
parser, semantic, compiler, VM, integration, and documentation changes together.
