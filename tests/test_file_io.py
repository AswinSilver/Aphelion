"""Direct UTF-8 file boundaries, independent of native argv code pages."""

import os
import subprocess

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)


PAYLOAD = b"\0\r\n\x1a\xffx"


@pytest.fixture(scope="session")
def file_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("file-io")
    support = ROOT / "tests" / "support"
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("file", "bytecode", "value"):
        output = build / (name + ".o")
        result = subprocess.run(
            [*flags, "-include", str(support / "fail_alloc.h"), "-c",
             str(ROOT / "src" / (name + ".c")), "-o", str(output)],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("file-checks")
    result = subprocess.run(
        [*flags, *objects, str(support / "fail_alloc.c"), str(support / "file_checks.c"),
         *math_libraries(), "-o", str(executable)],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def file_check(executable, directory, operation, path=None):
    command = [str(executable), operation]
    if path is not None:
        raw = path if isinstance(path, bytes) else str(path).encode("utf-8")
        command.append(raw.hex())
    result = subprocess.run(command, cwd=directory, capture_output=True,
                            text=True, encoding="utf-8", timeout=10)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "file checks passed\n"


def test_utf8_open_read_write_and_regular_file(file_executable, tmp_path):
    directory = tmp_path / "café Ω 雪 😀"
    directory.mkdir()
    source = directory / "input 雪😀.bin"
    source.write_bytes(PAYLOAD)
    file_check(file_executable, tmp_path, "read", source)
    file_check(file_executable, tmp_path, "regular", source)
    file_check(file_executable, tmp_path, "not-regular", directory)
    file_check(file_executable, tmp_path, "not-regular", directory / "missing")
    output = directory / "output Ω😀.bin"
    file_check(file_executable, tmp_path, "write", output)
    assert output.read_bytes() == PAYLOAD


def test_utf8_bytecode_api_roundtrip(file_executable, tmp_path):
    directory = tmp_path / "bytecode 雪😀"
    directory.mkdir()
    output = directory / "café Ω.apb"
    file_check(file_executable, tmp_path, "bytecode", output)
    assert output.read_bytes().startswith(b"APB1")


def test_file_errors_preserve_errno_and_release_conversions(file_executable, tmp_path):
    file_check(file_executable, tmp_path, "errors")


def test_file_conversion_allocation_failures(file_executable, tmp_path):
    source = tmp_path / "allocation 雪😀.bin"
    source.write_bytes(PAYLOAD)
    file_check(file_executable, tmp_path, "alloc", source)
    assert source.read_bytes() == PAYLOAD


@pytest.mark.skipif(os.name == "nt", reason="POSIX filenames are native byte strings")
def test_posix_non_utf8_filename_bytes_are_unchanged(file_executable, tmp_path):
    path = os.fsencode(tmp_path) + b"/native-\xff.bin"
    with open(path, "wb") as output:
        output.write(PAYLOAD)
    file_check(file_executable, tmp_path, "read", path)
    file_check(file_executable, tmp_path, "regular", path)
    file_check(file_executable, tmp_path, "write", path)
    with open(path, "rb") as source:
        assert source.read() == PAYLOAD
