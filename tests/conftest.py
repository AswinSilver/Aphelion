"""Shared builds; release/CI can test the actual binary instead of rebuilding it."""

import os
import re
import shlex
import shutil
import subprocess
from functools import wraps
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"


def reject_sanitizer_output(result):
    """Check both captured streams, including output attached to exceptions."""
    for stream in (result.stdout, result.stderr):
        diagnostic = stream or ""
        if isinstance(diagnostic, bytes):
            diagnostic = diagnostic.decode("utf-8", errors="replace")
        if (any(marker in diagnostic for marker in (
            "AddressSanitizer", "LeakSanitizer", "UndefinedBehaviorSanitizer"
        )) or re.search(r"\.(?:c|h|cc|cpp|cxx|hpp):\d+(?::\d+)?: runtime error:", diagnostic)):
            pytest.fail(f"sanitizer report in child process:\n{diagnostic}")


def checked_subprocess_run(original):
    @wraps(original)
    def checked_run(*args, **kwargs):
        # The CLI and compiler diagnostics use UTF-8, independent of the host's
        # Windows ANSI code page. Replacement keeps malformed diagnostic bytes
        # from concealing a sanitizer report behind UnicodeDecodeError.
        if (kwargs.get("text") or kwargs.get("universal_newlines")
                or kwargs.get("encoding") is not None or kwargs.get("errors") is not None):
            if kwargs.get("encoding") is None:
                kwargs["encoding"] = "utf-8"
            if kwargs.get("errors") is None:
                kwargs["errors"] = "replace"
        try:
            result = original(*args, **kwargs)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            reject_sanitizer_output(error)
            raise
        reject_sanitizer_output(result)
        return result

    return checked_run


@pytest.fixture(scope="session", autouse=True)
def reject_sanitizer_reports():
    """Cover session compiler fixtures as well as expected status-1 CLI errors."""
    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(subprocess, "run", checked_subprocess_run(subprocess.run))
        yield


def split_arguments(value):
    if not WINDOWS:
        return shlex.split(value)

    # Windows command lines use double quotes and only treat backslashes as
    # escapes immediately before a double quote (the inverse of list2cmdline).
    # POSIX shlex silently turns C:\\tools\\gcc.exe into C:toolsgcc.exe.
    arguments = []
    index = 0
    while index < len(value):
        while index < len(value) and value[index] in " \t":
            index += 1
        if index == len(value):
            break
        argument = []
        quoted = False
        while index < len(value) and (quoted or value[index] not in " \t"):
            backslashes = 0
            while index < len(value) and value[index] == "\\":
                backslashes += 1
                index += 1
            if index < len(value) and value[index] == '"':
                argument.append("\\" * (backslashes // 2))
                if backslashes % 2:
                    argument.append('"')
                elif quoted and index + 1 < len(value) and value[index + 1] == '"':
                    argument.append('"')
                    index += 1
                else:
                    quoted = not quoted
                index += 1
            else:
                argument.append("\\" * backslashes)
                if index < len(value) and (quoted or value[index] not in " \t"):
                    argument.append(value[index])
                    index += 1
        if quoted:
            raise ValueError("unclosed double quote in compiler arguments")
        arguments.append("".join(argument))
    return arguments


def compiler_command():
    value = os.environ.get("CC", "gcc")
    # CMake's compiler setting is a single file path, possibly unquoted and
    # containing spaces. Also support conventional CC='ccache gcc ...' commands.
    command = [value] if Path(value).is_file() else split_arguments(value)
    assert command and command[0], "CC must specify a compiler executable"
    return command


def test_cflags():
    return split_arguments(os.environ.get("APHELION_TEST_CFLAGS", ""))


def native_executable_name(stem):
    return stem + (".exe" if WINDOWS else "")


def diagnostic_path(path):
    """Return the canonical source path spelling used in diagnostics."""
    return Path(path).resolve().as_posix()


def math_libraries():
    return [] if WINDOWS else ["-lm"]


OPENSSL_HEADER = Path("include") / "openssl" / "evp.h"


def _has_openssl_headers(root):
    return (root / OPENSSL_HEADER).is_file()


def openssl_root():
    """Locate an OpenSSL prefix the test builds can compile and link against.

    Windows builds link against a MinGW/MSYS2 prefix rather than a system
    directory, so discover one from the environment or the compiler location.
    POSIX builds use the system default search path and return ``None``.
    """
    for variable in ("APHELION_OPENSSL_ROOT", "OPENSSL_ROOT_DIR"):
        override = os.environ.get(variable)
        if override is not None:
            assert override, f"{variable} is empty"
            root = Path(override)
            assert _has_openssl_headers(root), (
                f"{variable} does not contain {OPENSSL_HEADER.as_posix()}: {root}")
            return root
    if not WINDOWS:
        return None
    candidates = []
    prefix = os.environ.get("MINGW_PREFIX")
    if prefix:
        candidates.append(Path(prefix))
    compiler = shutil.which(compiler_command()[0])
    if compiler:
        # C:/msys64/ucrt64/bin/gcc.exe -> C:/msys64/ucrt64
        candidates.append(Path(compiler).resolve().parent.parent)
    for fallback in ("C:/mingw32/opt", "C:/msys64/ucrt64", "C:/msys64/mingw64",
                     "C:/msys64/clang64"):
        candidates.append(Path(fallback))
    for candidate in candidates:
        if _has_openssl_headers(candidate):
            return candidate
    return None


def crypto_include_flags():
    """Include paths for the vendored Argon2 headers and OpenSSL headers."""
    override = os.environ.get("APHELION_CRYPTO_CFLAGS")
    if override is not None:
        return split_arguments(override)
    flags = [f"-I{ROOT / 'third_party' / 'argon2' / 'include'}"]
    root = openssl_root()
    if root is not None:
        flags.append(f"-I{root / 'include'}")
    return flags


def crypto_libraries():
    """Link flags for the OpenSSL libcrypto runtime used by ACSL."""
    override = os.environ.get("APHELION_CRYPTO_LIBS")
    if override is not None:
        return split_arguments(override)
    libraries = []
    root = openssl_root()
    if root is not None and (root / "lib").is_dir():
        libraries.append(f"-L{root / 'lib'}")
    libraries.append("-lcrypto")
    return libraries


def acsl_sources():
    """Every ACSL translation unit, for fixtures that link the VM directly."""
    return sorted((ROOT / "src").glob("acsl*.c"))


def ensure_crypto_runtime_path():
    """Expose the discovered OpenSSL DLL directory to child processes."""
    root = openssl_root()
    if root is None:
        return
    binaries = root / "bin"
    if not binaries.is_dir():
        return
    entries = [str(binaries)]
    entries.extend(entry for entry in os.environ.get("PATH", "").split(os.pathsep)
                   if entry and Path(entry) != binaries)
    os.environ["PATH"] = os.pathsep.join(entries)


ensure_crypto_runtime_path()


@pytest.fixture(scope="session")
def aphelion_executable(tmp_path_factory):
    supplied = os.environ.get("APHELION_TEST_EXECUTABLE")
    if supplied is not None:
        assert supplied, "APHELION_TEST_EXECUTABLE is empty"
        path = Path(supplied).resolve()
        assert path.is_file(), f"APHELION_TEST_EXECUTABLE does not exist: {path}"
        return path
    output = tmp_path_factory.mktemp("build") / native_executable_name("aphelion")
    command = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
               "-Werror", *test_cflags(), f"-I{ROOT / 'include'}",
               *crypto_include_flags(),
               *(str(p) for p in sorted((ROOT / "src").glob("*.c"))),
               *math_libraries(), *crypto_libraries(), "-o", str(output)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", timeout=120)
    assert result.returncode == 0, f"build failed:\n{result.stdout}\n{result.stderr}"
    return output


@pytest.fixture(scope="session")
def strict_aphelion_executable(aphelion_executable):
    return aphelion_executable
