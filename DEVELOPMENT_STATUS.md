# Aphelion development status

## Current state — essential builtins and five-feature release gate locally complete

Final local verification on 2026-10-19 passed after implementing builtins,
immutable primitive lists, traverse, source modules, and the real fresh-scope
REPL, including the final native Windows Unicode argv/file-I/O fix and the
essential-builtin audit. The audit found and fixed one real gap: semantic
analysis rejected text/glyph ordered comparisons even though the VM had partial
support, and VM text ordering incorrectly stopped at embedded NUL bytes. The
latest results are below; earlier feature counts are staged historical
checkpoints.

### Essential builtin audit and practical program

All eight documented essential builtins are present in the lexer/parser,
semantic checker, compiler opcode path, VM, and tests:

```text
reveal(value) -> silent
receive() -> text
to_text(value) -> text
to_whole(value) -> whole
to_decimal(value) -> decimal
length(text-or-list) -> whole
kind(value) -> text
assert(signal[, text]) -> silent
```

Arity/type checks are semantic errors; runtime input, conversion, assertion,
allocation, and output failures return status 1. `reveal` accepts exactly one
value and writes one predictable line. `receive` strips LF and paired CR,
preserves other bytes including NUL, handles EOF/empty/long input, and reports
read/allocation failures. `to_whole` accepts whole/finite decimal/numeric text
with exact signed-64-bit bounds; `to_decimal` accepts whole/finite decimal/C
`strtod`-grammar text; `to_text` preserves text identity and embedded NULs.
`length` counts text bytes/list elements, `kind` returns stable names, and
`assert` eagerly evaluates an optional text message before succeeding or
reporting a located failure. `reveal` output failures are checked.

`examples/builtins_practical.agne` is a real end-to-end program using input,
both numeric conversions, `to_text`, output, length, kind, assertions, a
function, conditions, arithmetic, and a loop. With `Ada`, `7`, and `2.5` on
three input lines it prints:

```text
Hello Ada
3
whole
7 items
5
7
```

| Final check | Observed result |
| --- | --- |
| Native Windows strict optimized Make, fresh source-only copy in a spaced path | **853 passed, 9 skipped** |
| Native Windows CMake Release/CTest, workspace and fresh source copy | **1/1 CTest entry passed**; workspace inner pytest **853 passed, 9 skipped** |
| Fresh x86-64 Debian WSL2 strict Make/full suite | **857 passed, 5 skipped** |
| Fresh Linux CMake Release/CTest | **1/1 CTest entry passed**, inner pytest **857 passed, 5 skipped** |
| Fresh Linux ASan/UBSan with leak detection | **857 passed, 5 skipped**, no findings |
| Sanitized seed-2026 source/APB harnesses | **500 cases each passed**; 200,613 / 75,063 bytes |
| Portable release smoke and builtin examples | **24-check smoke passed**; all original shipped examples plus `builtins_practical.agne` checked/run/compiled/disassembled; redirected REPL sample passed |
| CI/build configuration | YAML and run-block Bash syntax, spaced compiler/artifact paths, compiler-only mode, and missing Python/pytest rejection passed |

Remote Windows UCRT64/macOS/Linux Actions and Valgrind remain unrun, not passes.
The local Windows binaries are native PE32/i386, not the x86-64 CI build or a
verified standalone package; `libmcfgthread-2.dll` is an observed runtime import.
See the final receipts/next action below and `docs/RELEASE_GATE.md`.

### Phase 1: pre-feature Windows release-gate checkpoint (historical)

The current host is native Windows 11 / Git Bash, **i686-w64-mingw32 GCC
16.1.0**, Python **3.11.9**, pytest **8.4.2**, and `mingw32-make`. This is not
the Debian environment recorded in the historical reliability pass below.
CMake **4.4.4** was installed in temporary tooling, not added as a project
dependency. `file aphelion.exe` now reports a native **PE32/i386 Windows**
executable; no MSYS/Cygwin runtime imports were found. This does not establish
that the separate x86-64 UCRT64 GitHub runner has passed.

Verified fixes include exact executable/compiler selection across Make/CTest,
Windows compiler paths containing spaces, explicit UTF-8 subprocess handling,
sanitizer checking during session fixtures and expected-error subprocesses,
and short depth-test parameter IDs (large IDs exceeded Windows' environment
variable limit). CMake now fails if tests are enabled but Python >=3.10 or
pytest is missing; `BUILD_TESTING=OFF` remains compiler-only. CI now includes
native macOS Clang and native Windows UCRT64 Make/CMake gates and checks
ELF/PE/Mach-O artifact identity. `tools/release_smoke.py` provides a portable
19-check CLI/example/input/bytecode/error smoke without modifying examples.

Pre-feature checks:

- Native optimized Make suite: **148 passed, 1 skipped** before adding the four
  release-smoke tests.
- A fresh source-only copy, without binaries, objects, caches, private app
  metadata, or example `.apb` files, in a path containing spaces: strict Make
  build and **152 passed, 1 skipped**; CMake Release build and **1/1 CTest entry
  passed**; the 19-check smoke passed. This is not a Git checkout: no `.git`
  exists here, so an actual clean clone remains a remote release gate.
- Native seed-2026 fuzzing: **500 source and 500 bytecode cases passed**,
  197,319 / 75,414 bytes respectively, five-second per-case timeout. Reports
  are in the approved temporary directory, not the repository.
- MinGW sanitizer build was attempted and **failed linking** because this
  installation lacks `-lasan` and `-lubsan`; no native sanitizer pass is claimed.
- Windows Make/CMake builds with compiler and output paths containing spaces,
  compiler-only configuration, and missing Python/pytest rejection passed.
- The one skipped test requires Unix `/dev/full`; it was not removed.

Commands included `mingw32-make CC=gcc OS=Windows_NT clean`,
`mingw32-make CC=gcc OS=Windows_NT CFLAGS=-O2 build`, and
`mingw32-make CC=gcc OS=Windows_NT test`. Native CMake used `-G "MinGW Makefiles"`,
explicit native GCC/Make/Python paths, `-DCMAKE_BUILD_TYPE=Release`,
`-DBUILD_TESTING=ON`, and `ctest -C Release --output-on-failure --no-tests=error`.
See `docs/RELEASE_GATE.md` for portable installation/reproduction and GitHub
Actions instructions.

Remote Windows UCRT64, Linux Actions, and macOS Actions have **not run** from
this session. There is no Git metadata/remote or `gh` installation, so no
workflow was dispatched and no source was committed/pushed. A local Debian
WSL distribution was discovered without development tools; its dependency
installation timed out, so no new Linux result is claimed at this checkpoint.
Valgrind is unavailable. This initial dependency-installation checkpoint is
superseded by the successful fresh Linux runs starting with Feature 2 below;
it is preserved to distinguish the earlier gap from subsequent verification.

### Feature 1: builtins completed and locally verified

Implemented real semantic/compiler/bytecode/VM `length`, `kind`, and `assert`
support. Text length counts bytes including NUL, kind returns stable owned
type-name text (including hollow/silent), and assert requires signal plus
optional eager text message. Failure diagnostics escape controls/NUL and are
bounded without allocating. Uninitialized reads still fail before observation.
Builtin names cannot be redeclared. New opcode numbers 40/41/42 are appended;
v1 serialized layout and older opcode numbers are preserved.

After the final builtin edits: `mingw32-make CC=gcc OS=Windows_NT test` reported
**310 passed, 1 skipped**. The strict optimized native build and CMake Release
build/CTest passed. `examples/builtins.agne` printed `6`, `text`, `hollow`;
the original examples and 19-check release smoke passed. Seed-2026 five-second
fuzz campaigns both passed 500 cases (source 196,508 bytes, bytecode 75,233
bytes), now including builtin source/operand cases. Runtime allocation-failure
sweeps cover kind/length/assert cleanup. Current native MinGW still cannot
link ASan/UBSan, so no fresh sanitizer pass is claimed here.

A real regression gap was corrected without removing coverage: opcode 31 in
the old frame-floor test was REVEAL, not RETURN. The suite now checks both
RETURN (30) and REVEAL (31).

### Feature 2: immutable lists completed; fresh Windows and Linux verification

Implemented the pre-recorded `docs/COLLECTIONS_DESIGN.md` contract: five exact
homogeneous primitive element types, annotated empty bindings, checked indexing,
immutable shared storage, sequence equality, length/kind/output/conversion,
function/scoped lifetimes, and defensive LIST/INDEX bytecode. Tags/opcodes are
appended (list tag 9, LIST 43, INDEX 44), with no recursive list constants or
change to the v1 layout. Nested/mutable lists remain deliberately unsupported.

Native Windows: **507 passed, 1 skipped**, strict optimized Make build,
CMake Release/CTest, original release smoke, and `examples/collections.agne`
all passed. Both seed-2026 campaigns passed 500 cases: source **196,063 bytes**,
bytecode **75,063 bytes**. Allocation-failure drivers exercise list construction,
element retention/indexing, formatting, rebinding, and error cleanup.

The slow Debian WSL toolchain installation subsequently completed. A fresh
source-only copy was built/tested independently (never reusing Windows objects):
**508 passed** normally and **508 passed** with ASan/UBSan/leak detection;
CMake Release/CTest and the 19-check CLI smoke passed. Both sanitized 500-case
campaigns passed with the same sizes above and no crashes/timeouts/findings.
Environment: Debian GCC **12.2.0**, Make **4.3**, CMake **3.25.1**, Python
**3.11.2**, pytest **7.2.1**, x86-64 Linux under WSL2. This is a real local
Linux run, not a GitHub Actions or macOS run. The documented Linux-only
`-no-pie` sanitizer workaround was used; sanitizers were not disabled.

Reproduction helper outside the repository:
`MSYS_NO_PATHCONV=1 wsl.exe -d OmniRushBox --exec bash /mnt/c/Users/awinr/AppData/Local/Temp/omnirush/aphelion_linux_gate.sh`.
Its successful source-copy/report directory is
`C:/Users/awinr/AppData/Local/Temp/omnirush/aphelion-linux-gate-kta8dr`.
The MSYS variable prevents Git Bash from rewriting the Linux script path.
Native Windows `aphelion.exe` remains PE32; Linux binaries stay in that separate
temporary copy. No Valgrind or remote/macOS pass is claimed.

### Feature 3: traverse completed and verified

Implemented the syntax/scope contract recorded before code. Traversal evaluates
the iterable once, retains an immutable snapshot, uses an immutable loop-local
element declaration, supports nested cycle/traverse control and shadowing, and
advances on skip. Lowering uses existing VM instructions and clears the element,
snapshot, and index locals on exit. Fault injection and direct post-loop slot
probes check cleanup; bounded VM tests check runaway body/loop work.

Native Windows strict Make/full suite: **645 passed, 1 skipped**; CMake Release
and CTest passed. `examples/traverse.agne` printed `1`, `3`, `4`; the 19-check
release smoke passed. Fresh Linux source-copy Make/normal suite: **646 passed**;
CMake/CTest passed; ASan/UBSan/leak suite: **646 passed** in 26.45s. Both
sanitized seed-2026 500-case campaigns passed, source **201,213 bytes**, bytecode
**75,063 bytes**. Successful Linux staging/report directory:
`C:/Users/awinr/AppData/Local/Temp/omnirush/aphelion-linux-gate-ezwtmk`.

### Feature 4: source modules completed and verified

Implemented the pre-recorded root-relative module design using independent
parses/AST transfers, canonical boundary/link checks, canonical deduplication,
cycles, shared exported namespace/collision diagnostics, deterministic once-only
initialization, and limits of 256 files/64 active/64 MiB aggregate. SOURCE=45
preserves filenames per call frame and through standalone `.apb` execution.
Loader/compiler/VM fault sweeps cover partial cleanup; missing/cycle diagnostics
identify both importer and target. No source concatenation or regex imports.

Final module Windows gate: **739 passed, 5 skipped**; strict optimized Make,
CMake Release/CTest, and 19-check smoke passed. `examples/modules.agne` printed
`module initialized` once despite two imports, followed by `42`.
Fresh Linux Make/full suite and ASan/UBSan/leak suite each reported **742 passed,
2 skipped**; CMake/CTest passed. Both sanitized seed-2026 500-case campaigns
passed: source **200,613 bytes**, bytecode **75,063 bytes**. Linux stage/reports:
`C:/Users/awinr/AppData/Local/Temp/omnirush/aphelion-linux-gate-EYABxV`.
Windows skips cover unavailable symlink/FIFO capabilities and Unix `/dev/full`;
Linux skips are Windows-only junction/case-identity checks. Windows junction
escape and case-insensitive deduplication ran and passed natively.

### Feature 5: real REPL completed and final filename defect fixed

The REPL is implemented: one physical line per fresh program/scope, real
compiler expression echo, no persistent declarations/multiline claim, recovery,
help/quit/EOF, 64 KiB/NUL draining, shared receive stdin, and cleanup/OOM tests.
Expression echo is a default-off compiler option using the real bytecode/VM,
not source wrapping or an AST interpreter. The sample
`aphelion.exe repl < examples/repl_lines.agne` printed `3`, `6`, `6`, `1`, `2`.
The full suite also runs 64 bounded seed-2026 source inputs through the REPL.

The pre-filename-check milestone was Windows **830 passed, 8 skipped** and
Linux normal/sanitized **836 passed, 2 skipped**, saved in the temporary Linux
stage `aphelion-linux-gate-9PGa7X`. These are not the final suite counts.

Final filename regressions initially reproduced **2 native Windows failures**:
narrow CRT argv/file operations corrupted Unicode source/module-root paths.
This is now fixed. `main.c` owns a checked UTF-8 copy of the native wide command
line; `file.h`/`file.c` provide wide Windows source/APB opens/stat and preserve
POSIX filename bytes. Make/CMake link Windows `shell32` without requiring
`-municode` or changing the direct GCC workflow. Tests cover Unicode and
supplementary characters, quotes/spaces, modules, diagnostics, APB roundtrips,
invalid UTF-16/UTF-8, and allocation-failure cleanup without truncating files.

The final full native/clean-source/Linux/sanitizer results are in the table at
the top. Windows skips are the three unprivileged symlink cases, FIFO, two
`/dev/full` cases, POSIX broken-pipe/PTY, and native-byte POSIX filenames. Linux
skips are the two Windows module-identity/junction cases, two invalid-UTF-16
argument cases, and native Windows startup-allocation handling. Windows junction
escape, case identity, UTF-16 validation and startup-allocation sweeps passed;
the complementary POSIX checks ran on Linux.

### Final receipts and reproduction

Working folder: `C:/Users/awinr/.vscode/aphelion` (Git Bash equivalent
`/c/Users/awinr/.vscode/aphelion`). Final Windows clean-source Make took 51.88s;
its CMake/CTest run passed in 51.79s. Fresh Linux Make took 26.54s, CTest's
inner suite 28.07s, and ASan/UBSan/leak pytest 51.77s. The final direct strict
GCC build also passed; the Unicode/file boundary subset reported **17 passed,
1 skipped**, followed by the passing 24-check smoke.

Commands actually used from the project root (Git Bash):

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
APHELION_TEST_EXECUTABLE="$PWD/aphelion.exe" python -m pytest -q -rs tests/test_unicode_paths.py tests/test_file_io.py
python tools/release_smoke.py --executable ./aphelion.exe

# These local verification helpers/tooling are outside the repository.
gate='C:/Users/awinr/AppData/Local/Temp/omnirush'
python "$gate/aphelion_clean_source_gate.py"
python "$gate/validate_aphelion_gates.py"
MSYS_NO_PATHCONV=1 wsl.exe -d OmniRushBox --exec bash /mnt/c/Users/awinr/AppData/Local/Temp/omnirush/aphelion_linux_gate.sh
```

The Windows clean-source helper deliberately omits binaries, objects/caches,
`.apb` files and private app metadata, builds in a spaced path, tests the exact
Make/CMake artifacts, runs the smoke, and removes its temporary copy. It is not
a Git clone. The configuration helper's spaced-compiler C-driver probe selects
the 8 core checks; it is not reported as another full-suite pass. Its missing
Python/pytest configurations failed as expected, and compiler-only mode built.

Retained full-suite CTest receipts:

- Windows: `build Release/Testing/Temporary/LastTest.log`.
- Latest Linux source-copy/report root:
  `C:/Users/awinr/AppData/Local/Temp/omnirush/aphelion-linux-gate-JaHMx2`.
  Its `build-linux/Testing/Temporary/LastTest.log` records the full CMake suite;
  `source-fuzz.json` and `bytecode-fuzz.json` each record `passed: true`, 500
  executed cases, seed 2026, and no failures. These are temporary evidence, not
  source deliverables. The earlier post-fix stage `aphelion-linux-gate-0ZGqXr`
  also has passing CTest/fuzz receipts.

Linux used `make CC=gcc LDFLAGS=-no-pie sanitize`, then full pytest and both
five-second-per-case 500-case harnesses with the following environment/commands
from its separate Linux source-copy root:

```sh
APHELION_TEST_EXECUTABLE="$PWD/aphelion-sanitize"
APHELION_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g -no-pie'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1
export APHELION_TEST_EXECUTABLE APHELION_TEST_CFLAGS ASAN_OPTIONS UBSAN_OPTIONS
python3 -m pytest -q
python3 tools/fuzz_harness.py source --executable ./aphelion-sanitize --cases 500 --seed 2026 --timeout 5 --report source-fuzz.json
python3 tools/fuzz_harness.py bytecode --executable ./aphelion-sanitize --cases 500 --seed 2026 --timeout 5 --report bytecode-fuzz.json
```

Run this Linux block only in a separate source-copy/build root, as the helper
does, never against reused Windows objects. The documented `-no-pie` override
addresses local ASan startup behavior, not a disabled sanitizer.

The original nine examples were previously checked/run/compiled/disassembled
in Unicode temporary paths; the new `builtins_practical.agne` was checked,
run with input, compiled, disassembled, and executed again from serialized
bytecode. The 24-check smoke now covers that practical program on the current
Windows and Linux Make/CMake/sanitized artifacts. Workspace examples were not
rewritten. `file` and `objdump` confirmed both Windows artifacts are PE32/i386
without MSYS/Cygwin imports; Linux artifacts are x86-64 ELF. The observed
Windows `libmcfgthread-2.dll` import is documented in `docs/RELEASE_GATE.md`;
no standalone clean-machine binary-package pass is claimed.

Strict C compilation/type checking and warnings-as-errors passed. There is no
separate configured lint/static-analysis check; Valgrind was not available.
Generated binaries/objects/dependencies, build directories, caches and the old
`examples/factorial.apb` remain ignored local artifacts. No new fuzz reports
were written in the workspace; private `/.omnirush/` is ignored and excluded
from source-copy verification. No source commit, push or repository initialization
was performed because this workspace has no Git metadata/remote or `gh`.

### Remaining release action

All five requested features and the local regression gate are complete. The
next concrete action is to publish the reviewed source to the intended GitHub
repository/branch and run **Actions > CI > Run workflow**, then record the run
URL, commit SHA, compiler versions and actual four-job outcomes here. Exact
commands/requirements are in `docs/RELEASE_GATE.md`; this requires the user's
repository choice/access and is not a completed remote run. Address actual
UCRT64/macOS/Linux runner failures before claiming cross-platform release.
Before distributing Windows binaries, also resolve and test their runtime-DLL
packaging on a clean machine. Native Windows sanitizer libraries and Valgrind
remain separate environment gaps. No active subagent work is left to integrate.

## Prior reliability/portability stage — historical results

The input/conversion stage below was independently baselined at **33 passing
tests**, a warning-clean strict build, three working examples, and working
`run/check/compile/tokens/ast/disasm/help/version` commands. The repository is
not currently initialized as a Git repository; no commit was created.

### Fixed verified defects

- A 20,000-level parenthesized expression crashed the host parser. Parser
  recursion and tree depth now have controlled 128-level limits (including
  left-associative chains), bounding later recursive passes and destruction.
- A condition beginning with a grouped subexpression was incorrectly treated
  as a completely parenthesized condition. `when`/`cycle` now parse the normal
  expression grammar, with a regression test for `(n + 1) < 3`.
- Whole equality incorrectly rounded both operands through double. It is now
  exact, and whole overflow checks use integer bounds rather than `long double`
  precision. Minimum whole literals are accepted correctly; conversion bounds
  use exact powers of two, independently of the host long-double format.
- Decimal annotations, parameters, assignments, and returns did not actually
  widen whole values. Code generation now inserts the existing conversion
  instruction so static and runtime types agree.
- CRLF input retained CR; escaped NUL text was silently truncated in compilation,
  conversions, and serialization. Lengths are now preserved, and raw NUL source
  bytes are diagnosed instead of silently ending the file.
- Partial AST nodes leaked on failed vector appends and malformed prefix types.
  The parser now releases unattached subtrees. Allocation failure when creating
  the entry function dereferenced a null function table; this was reproduced by
  fault injection/UBSan and fixed with immediate cleanup and a failure result.
- Diagnostic allocation failure is sticky instead of disappearing. Text OOM
  checks inspect the value tag rather than an inactive union pointer member.
- AST literal display avoids temporary allocations and preserves escaped NULs.
  Disassembly now returns failure when constant formatting runs out of memory,
  instead of reporting success with an `<oom>` placeholder; fault injection
  checks both the loader and this formatting path.
- The loader now bounds files and validates remaining bytes before allocating,
  preserves binary text, rejects duplicate/NUL global metadata, invalid glyph
  scalars/nonfinite constants, invalid entry arity, and unused operands. Signed
  integer decoding no longer relies on implementation-defined signed casts.
- Operand pops previously could consume frame-local slots in hostile bytecode.
  They now enforce the frame floor. VM initialization has a distinct sentinel,
  and early global reads, invalid logical operands, stack exhaustion, and runaway
  instruction streams fail in a controlled way.

### Infrastructure and tests

- Added shared prebuilt-binary test support, broad numeric/semantic/lifetime
  regressions, test-only C allocation-failure injection for the pipeline, loader,
  and disassembler, plus bounded hostile VM/bytecode read-write-execute checks.
- Added deterministic source and bytecode harnesses in `tools/fuzz_harness.py`:
  seeds, size/case/time limits, exact base64 inputs/hashes, reproducible JSON
  reports, and detection of sanitizer diagnostics as failures.
- Make now builds/tests/cleans/sanitizes with strict warnings and header
  dependencies. CMake builds and CTest test the selected executable. CI specifies
  Ubuntu strict/CMake/sanitizer jobs and Windows MinGW tests; remote jobs have
  **not** been run from this chat.
- Quick start and reliability contract: `docs/QUICKSTART.md` and
  `docs/RELIABILITY.md`. The language/architecture/bytecode docs were synchronized.

### Current verification and environment

The current verification environment is Debian with GCC 12.2 and GNU Make 4.3.
Python/pytest and CMake 3.25 were installed in the container for validation.
An attempted broader CMake/Valgrind download timed out; CMake was subsequently
installed separately. Valgrind remains unavailable. MinGW sanitizer libraries
were unavailable in the earlier Windows environment.

The normal and ASan/UBSan regression suites currently report **112 passed**.
The unsanitized direct build, Make build/test, and CMake Release build/CTest have
all completed successfully in this environment. The test runner rejects
sanitizer reports even when a language-error case expects status 1.

Latest final results after the last code/test edits:

| Check | Observed result |
| --- | --- |
| Direct strict GCC build | Passed, no warnings |
| Optimized Make build/test | 112 passed in 3.41s |
| CMake Release build/CTest | 1/1 entry passed; inner pytest: 112 passed in 3.43s |
| ASan/UBSan with leak detection | 112 passed in 6.50s; no sanitizer findings |
| Seed-2026 sanitized source corpus | 500 cases: 3 successful, 497 normal diagnostics |
| Seed-2026 sanitized bytecode corpus | 500 cases: 80 successful, 420 normal diagnostics |
| CLI/examples/input/error smoke | Passed for direct, Make, and CMake Linux binaries |

Verified commands (from the project root):

```sh
make clean
make CFLAGS=-O2 build
make test
# 112 passed (latest full suite)
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
# 1/1 CTest entry passed; that entry runs the full pytest suite
```

Sanitizer verification used the following local Linux workaround:

```sh
make LDFLAGS=-no-pie sanitize
APHELION_TEST_EXECUTABLE="$PWD/aphelion-sanitize" \
APHELION_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g -no-pie' \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
python -m pytest -q
# 112 passed; no sanitizer/leak findings in this finite suite
```

Default PIE sanitizer binaries intermittently failed at startup in this
container (also reproduced on a trivial C program), so `-no-pie` was used only
as a documented local validation override, not to disable any sanitizer.

With `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`, both harnesses passed 500 cases with seed 2026:

```sh
python tools/fuzz_harness.py source --executable ./aphelion-sanitize --cases 500 --seed 2026 --timeout 5 --report /tmp/aphelion-source-final.json
python tools/fuzz_harness.py bytecode --executable ./aphelion-sanitize --cases 500 --seed 2026 --timeout 5 --report /tmp/aphelion-bytecode-final.json
```

Source corpus: 197,319 bytes. Bytecode corpus: 75,414 bytes. Both reports have
`passed: true`, 500 executed cases, and no crashes, timeouts, or sanitizer
diagnostics. Reports are temporary generated evidence, not committed inputs.
All three examples and the advertised CLI commands were re-exercised; input
smoke output was `Hello Aswin` followed by `43`, with status 0. Bytecode tests
also read, rewrite, compare, and execute compiled binary-text/minimum-whole data.
Smoke checks verified status 1 for a runtime error and status 2 for invalid
usage/the deferred REPL.

Generated artifacts were reviewed: binaries, object/dependency files, CMake
output, caches, and `examples/factorial.apb` are covered by `.gitignore` and
remain available for local use. No fuzz JSON reports were written into the
project. The current `aphelion.exe` was built by Linux GCC and is a Linux
binary despite its suffix, not a native Windows release executable.

### Boundaries and next steps

This is not a memory-safety proof or exhaustive fuzz campaign. Allocation
injection covers selected valid/error programs and a compiled file, not all
possible failure combinations or C-library internals. Complete UTF-8 validation,
multibyte glyph decoding, compile-time anchor folding, and static interprocedural
global-initialization analysis remain incomplete. Frame slots retain text until
function end. No thread safety, GC, collections, modules, or REPL is claimed.
The final changed code has not been executed on Windows/macOS in this session;
the Windows CI job is configured to validate it on a real runner. The local
reliability stage is complete; the next release gate is to run the configured
CI/platform checks and address their actual results, not to claim portability
from a Linux build alone.

## Prior input/conversion stage (historical record)

## Audit snapshot

The existing repository was inspected before changes in this development pass.
It is a modular C11 compiler/VM project under `src/` and `include/aphelion/`,
with three `.agne` examples and a pytest integration suite.

### Verified commands

The strict direct build completed successfully:

```text
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
```

The existing suite completed with 14 passing tests:

```text
pytest -q -p no:cacheprovider
14 passed
```

The three checked-in examples executed successfully:

- `examples/control_flow.agne` -> `1`, `2`, `4`, `5`, `6`, `7`
- `examples/factorial.agne` -> `120`
- `examples/hello.agne` -> greeting and final score

The advertised `--help`, `--version`, `run`, `check`, `compile`, `tokens`,
`ast`, and `disasm` commands were exercised successfully.  `repl` correctly
returns status 2 with an explicit not-implemented message.  The installed
environment has GCC and Python/pytest; `make`, CMake, and MinGW sanitizer
libraries are not available, so Make/CMake builds and ASan/UBSan runs remain
environment gaps rather than passes.

### Findings before this pass

1. `receive` was lexed as a reserved keyword but rejected by the parser, so the
   advertised vocabulary had no working input path.
2. `to_text` existed, but numeric conversions were not implemented and the
   documentation still described all conversions as future work.
3. Semantic analysis inferred `whole / whole` as `whole`, while the VM returned
   a decimal for every division.  That could place a runtime value with a type
   different from its statically checked binding.  Unary negation of the
   minimum 64-bit integer similarly returned a decimal instead of reporting
   overflow.
4. The semantic pass supports nested shadowing, but the bytecode compiler
   originally resolved locals by name only.  An inner declaration could
   therefore capture later references that semantically belonged to an outer
   declaration.
5. `.apb` loading checked the header and table sizes, but did not fully reject
   unknown opcodes, invalid instruction operands, invalid jump targets, or
   trailing bytes before disassembly/execution.

## Completed in this pass

- Added `receive()` through parser, semantic analysis, bytecode, and VM.  It
  reads a complete line from stdin, strips `\n` and paired `\r`, grows storage
  for long lines, returns empty text for an empty line or EOF, and reports
  input/allocation failures as runtime errors.
- Added checked `to_whole()` and `to_decimal()` builtins.  Text conversions
  reject invalid syntax, whitespace, non-finite values, and overflow; decimal
  to whole truncates toward zero when representable.  `to_text()` remains
  available for all runtime values.
- Fixed whole-number division so `whole / whole` produces a whole result, made
  integer negation overflow a controlled runtime error, and made whole-number
  comparisons exact instead of converting both operands to double.
- Fixed local bytecode slot resolution to use semantic declaration/parameter
  identity, preserving nested shadowing rather than resolving locals by name,
  including declarations nested in top-level blocks.
- Added expression newline continuation after operators/opening delimiters and
  hardened `.apb` loading against unsupported opcodes, out-of-range operands,
  invalid jump targets, invalid function metadata, invalid function constants,
  and trailing data.
- Expanded the integration suite from 14 to 33 passing tests, covering input,
  empty/EOF/long lines, conversions, nested shadowing, whole division,
  multiline expressions, numeric overflow, and corrupt bytecode.

## Latest verification

```text
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c -lm -o aphelion.exe
pytest -q
33 passed
```

All three examples still run successfully.  A piped stdin smoke program
returned `input=from-stdin` with exit status 0, and compile/disasm round-tripped
`examples/factorial.agne` successfully.

A subprocess malformed-source smoke generated 262 inputs (including malformed
comments, literals, delimiters, operators, and random text) with zero crashes
or timeouts.

## Remaining limitations and next steps at the historical 33-test checkpoint

At that earlier input/conversion checkpoint, the REPL, collections/traverse,
modules/summon, and the remaining `length`, `kind`, and `assert` builtins were
still deliberately deferred. Make/CMake,
Valgrind, and ASan/UBSan could not be run in this environment because their
tools/libraries are unavailable; the strict direct GCC build and subprocess
fuzz smoke are the available checks.  The next feature should be a narrowly
scoped REPL only after deciding how persistent declarations and multiline input
will be represented, followed by collections and modules.
