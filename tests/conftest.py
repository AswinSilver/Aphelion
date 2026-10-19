"""Shared builds; release/CI can test the actual binary instead of rebuilding it."""

import os
import re
import shlex
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


def math_libraries():
    return [] if WINDOWS else ["-lm"]


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
               *(str(p) for p in sorted((ROOT / "src").glob("*.c"))),
               *math_libraries(), "-o", str(output)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", timeout=120)
    assert result.returncode == 0, f"build failed:\n{result.stdout}\n{result.stderr}"
    return output


@pytest.fixture(scope="session")
def strict_aphelion_executable(aphelion_executable):
    return aphelion_executable
