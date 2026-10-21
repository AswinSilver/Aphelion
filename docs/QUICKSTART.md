# Quick start

Aphelion is a C11 compiler and stack bytecode VM. There is no Python runtime
inside the language; Python is needed only for development tests and fuzzing.

## Install build tools

**Windows 11:** install a MinGW-w64 GCC toolchain (for example MSYS2 UCRT64),
put its `bin` directory on PATH, and open its terminal in this project. For
Make/CMake use their MinGW-compatible versions. MSVC and macOS builds are not
validated by the current release checks.

**Debian/Ubuntu:** install `gcc`, `libc6-dev`, `make`, and optionally `cmake`.
Use Python 3.10+ with pytest for development checks (a virtual environment is
recommended).
Exact native Windows/Linux package and CI commands are in `RELEASE_GATE.md`.

```sh
python -m pip install pytest
```

## Compile

From the project root, the original direct workflow still works:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
```

In PowerShell, which does not expand wildcards for every native compiler,
expand the source list explicitly, or use the Make/CMake alternatives below:

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude (Get-ChildItem src/*.c).FullName -lm -o aphelion.exe
```

On Linux, naming the output `aphelion` instead of `aphelion.exe` is conventional.
An `.exe` suffix does not change the target platform: a Linux GCC build is not
a native Windows executable. Rebuild with the native toolchain on each platform
and use separate CMake build directories when switching platforms.

```sh
make build
make test
make clean

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Make chooses `.exe` when `OS=Windows_NT`; `CC`, `TARGET`, `PYTHON`, and compiler
flags can be overridden. Tests launched by Make and CTest use the binary just
built, not an unrelated executable from PATH. Use `mingw32-make` when that is
the native Make provided by your Windows toolchain. Test-enabled CMake requires
Python >=3.10 and pytest; use `-DBUILD_TESTING=OFF` only for compiler-only builds.
Windows binaries may need toolchain runtime DLLs; see `RELEASE_GATE.md` before
distributing them.

## Run a program

```sh
./aphelion.exe run examples/hello.agne
./aphelion.exe run examples/factorial.agne
```

PowerShell uses `.\aphelion.exe` instead. Expected output for `hello.agne`:

```text
Welcome Aswin
Final score: 100
```

Create a file `hello_input.agne`:

```agne
forge name: text = receive()
reveal("Hello " + name)
forge count: whole = to_whole("42")
reveal(count + 1)
```

Run it, enter a name, and press Enter. Input excludes the line ending. Empty
input/EOF returns empty text. Invalid numeric text causes a runtime failure,
not a partial conversion. See `LANGUAGE_SPEC.md` for exact conversion rules.

## Check and debug

```sh
./aphelion.exe check examples/hello.agne
./aphelion.exe tokens examples/hello.agne
./aphelion.exe ast examples/hello.agne
./aphelion.exe compile examples/hello.agne
./aphelion.exe disasm examples/hello.apb
```

`check` never executes the program. `compile` writes `.apb` beside the source;
`disasm` validates and displays that file. The public CLI does not currently
run `.apb` files directly (the C VM API and round-trip tests do).

Errors are written to stderr as `file:line:column: message`. Exit status 0 is
success, 1 is a program/file/runtime failure, and 2 is invalid CLI usage or a
deferred command. Primitive lists, traversal, file modules, and a one-line REPL
are implemented; nested/mutable collections and persistent/multiline REPL state
remain deferred.

## Try the new language features

```sh
./aphelion.exe run examples/builtins.agne
printf 'Ada\n7\n2.5\n' | ./aphelion.exe run examples/builtins_practical.agne
./aphelion.exe run examples/collections.agne
./aphelion.exe run examples/traverse.agne
./aphelion.exe run examples/modules.agne
./aphelion.exe repl
```

The first REPL uses a fresh scope per physical line: `forge n = 3; n * 2`
prints 6, but `n` on the next line is undefined. Use `:help`, `:quit`, or EOF.
It does not buffer multiline input; `summon` stays file-only. A redirected
sample needs no terminal interaction:

```sh
./aphelion.exe repl < examples/repl_lines.agne
```

Expected lines: `3`, `6`, `6`, `1`, `2`. On Linux use `./aphelion` for a
Make build. See `REPL.md`, `COLLECTIONS_DESIGN.md`, `MODULES_DESIGN.md`, and
`RELEASE_GATE.md` for boundaries and native installation/CI commands.

## Reproducible reliability checks

The following paths use the Unix Make output. On Windows use `./aphelion.exe`
and, if supported by the toolchain, `./aphelion-sanitize.exe` instead.

```sh
python -m pytest -q
python tools/release_smoke.py --executable ./aphelion
python tools/fuzz_harness.py source --executable ./aphelion --cases 500 --seed 2026
python tools/fuzz_harness.py bytecode --executable ./aphelion --cases 500 --seed 2026
```

Each fuzz mode bounds case count, bytes, and per-case execution time. Add
`--report /path/to/report.json` to preserve exact base64 inputs, hashes,
diagnostics, and seeds. Generated inputs use a temporary directory. Expected
compiler/loader errors are acceptable; crashes, timeouts, and sanitizer
diagnostics fail the harness. Bytecode fuzzing validates/disassembles inputs;
hostile VM execution is covered separately by bounded C tests.

On a sanitizer-capable GCC/Clang toolchain:

```sh
make sanitize
APHELION_TEST_EXECUTABLE="$PWD/aphelion-sanitize" \
APHELION_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
python -m pytest -q
```

The environment assignments above are for a POSIX shell. PowerShell can set
the corresponding `$env:NAME` variables when its toolchain supports these
sanitizers. CMake also supports `-DAPHELION_ENABLE_SANITIZERS=ON` in a separate
build directory. Missing sanitizer runtimes cause a real build failure.

This container had intermittent ASan startup failures even for a trivial C
program when PIE was enabled. Its verified local workaround was
`make LDFLAGS=-no-pie sanitize` and adding `-no-pie` to
`APHELION_TEST_CFLAGS`; do not apply this platform-specific workaround blindly
on Windows. A passing finite test run is not a proof of memory safety.
