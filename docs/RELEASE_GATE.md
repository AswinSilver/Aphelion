# Cross-platform release gate

Configured jobs are not evidence of successful native runs. See the current
section of `../DEVELOPMENT_STATUS.md` for actual environment/results. A native
Windows i686 MinGW build and a fresh x86-64 Debian WSL2 build have passed
locally, including the latter's ASan/UBSan/leak checks. The distinct UCRT64
x86-64, macOS, and GitHub Linux jobs still need execution. No memory-safety
proof or exhaustive fuzzing is claimed.

## Native Windows (MSYS2 UCRT64)

Install MSYS2, open its **UCRT64** terminal, update it (reopen if requested),
then install the actual native packages used by CI:

```sh
pacman -Syu
pacman -S --needed file make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-pytest
gcc -dumpmachine                 # expect x86_64-w64-mingw32
make OS=Windows_NT CC=gcc build
make OS=Windows_NT CC=gcc test
python tools/release_smoke.py --executable ./aphelion.exe
cmake -S . -B build-windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-windows --config Release --parallel
ctest --test-dir build-windows -C Release --output-on-failure --no-tests=error
file aphelion.exe
objdump -p aphelion.exe
```

Use native UCRT64 GCC/Python, not MSYS/Cygwin compilers or an unrelated Python.
The workflow checks the compiler triple and PE x86-64 imports. A local
32-bit MinGW toolchain can use `mingw32-make` instead of `make`, but its result
does not verify the configured 64-bit CI toolchain. See `QUICKSTART.md` for
the direct GCC/PowerShell source-list command. An `.exe` suffix alone does not
establish native Windows support. Rebuild after switching platforms; never
reuse another platform's objects or CMake cache.

### Native is not necessarily standalone

Inspect imports for each actual release toolchain. The locally verified i686
GCC 16.1.0 binaries import `libmcfgthread-2.dll` as well as Windows CRT/system
DLLs; they have no MSYS/Cygwin runtime imports. They run with the toolchain on
PATH, but have not been validated as a standalone distribution on a clean
Windows machine. The separate UCRT64 job may produce a different import set.

Before distributing binaries, supply any required redistributable toolchain
DLLs with their licenses, or use and verify an appropriate self-contained
link configuration. Then run the portable release smoke on a clean target
without the developer toolchain on PATH. The full pytest suite separately
requires a C compiler for its native test drivers. This session did not change
link flags to hide dependencies or claim that packaging gate passed.

Windows arguments and source/APB file operations use checked native wide APIs
with UTF-8 at the C boundary. Unicode/spaced filenames and module roots are
regression-tested; no active ANSI-code-page workaround is needed for paths.
Source text/identifier/glyph support remains as specified in `LANGUAGE_SPEC.md`.

## Linux

For Debian/Ubuntu, install a compiler **and C development headers**, Make,
CMake, Python >=3.10/pytest, and the artifact inspection tools. For example:

```sh
sudo apt-get update
sudo apt-get install gcc libc6-dev make cmake python3 python3-pytest binutils file
make CC=gcc PYTHON=python3 build
make CC=gcc PYTHON=python3 test
python3 tools/release_smoke.py --executable ./aphelion
cmake -S . -B build-linux -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux --config Release --parallel
ctest --test-dir build-linux -C Release --output-on-failure --no-tests=error
```

Sanitizer commands and the documented container-specific PIE workaround are
in `QUICKSTART.md`. Missing sanitizer libraries must fail, not disable checks.

## macOS

Install Apple's Command Line Tools with `xcode-select --install`, plus CMake
and Python >=3.10 with pytest (for example a Python virtual environment).
The GitHub macOS image supplies Xcode/Clang, Make, and CMake; setup-python and
pip supply the selected Python and pytest.

```sh
python -m pip install pytest
make CC="$(xcrun --find clang)" PYTHON=python build
make CC="$(xcrun --find clang)" PYTHON=python test
python tools/release_smoke.py --executable ./aphelion
cmake -S . -B build-macos -DCMAKE_C_COMPILER="$(xcrun --find clang)" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-macos --config Release --parallel
ctest --test-dir build-macos -C Release --output-on-failure --no-tests=error
file aphelion                  # expect native Mach-O
```

These commands/jobs are configured, not an assertion of a macOS run here.

## Run the actual GitHub gates

This workspace currently has no `.git`, remote, or `gh`, so a remote run cannot
be dispatched here. Publish the reviewed source and `.github/workflows/ci.yml`
to the intended GitHub repository/branch; do not publish generated binaries,
private `.omnirush` state, credentials, or temporary reports. This session has
not committed, initialized a repository, or pushed anything.

On GitHub, select **Actions > CI > Run workflow**, choose that branch, and run
it. `workflow_dispatch` must be available on the repository's default branch.
Alternatively, from an authenticated GitHub CLI terminal:

```sh
gh workflow run ci.yml --repo OWNER/REPO --ref BRANCH
gh run list --repo OWNER/REPO --workflow ci.yml --limit 5
gh run watch RUN_ID --repo OWNER/REPO --exit-status
gh run view RUN_ID --repo OWNER/REPO --log-failed
```

Review all four jobs: Windows MinGW, macOS Clang, Linux build/test, and Linux
sanitizers. Record the run URL, commit SHA, compiler versions, test/skip counts,
and failures in `DEVELOPMENT_STATUS.md`. Do not infer a pass from workflow YAML
parsing, a cross-compile, a Linux binary named `.exe`, or the local source-copy
test. A source-only temporary copy was tested locally; an actual clean Git
clone is still checked by Actions' checkout/build steps.
