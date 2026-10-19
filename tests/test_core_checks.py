"""Direct C pipeline checks for OOM cleanup and hostile VM instruction streams."""

import subprocess
from pathlib import Path

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)
from test_reliability import apb
from test_aphelion import run_cli, write_program


@pytest.fixture(scope="session")
def core_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("core")
    support = ROOT / "tests" / "support"
    objects = []
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    for source in sorted((ROOT / "src").glob("*.c")):
        if source.name == "main.c":
            continue
        output = build / (source.stem + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(source), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("core-checks")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"),
                             str(support / "core_checks.c"), *math_libraries(),
                             "-o", str(executable)], cwd=ROOT,
                            capture_output=True, text=True, encoding="utf-8", timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def test_allocation_failures_release_partial_state(core_executable):
    result = subprocess.run([str(core_executable)], capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    assert result.returncode == 0, result.stderr[-4000:]
    assert "cleanup checks passed" in result.stdout


@pytest.mark.parametrize(("instructions", "locals_count", "error"), [
    ([(4, 0, 0), (0, 0, 0)], 1, "underflow"),  # POP must not eat local slots
    ([(7, 0, 0), (0, 0, 0)], 1, "underflow"),  # STORE_LOCAL requires an operand
    ([(30, 0, 0)], 1, "underflow"),  # RETURN cannot consume a local
    ([(31, 0, 0)], 1, "underflow"),  # REVEAL cannot consume a local
    ([(26, 0, 0)], 0, "instruction limit"),
    ([(0, 0, 0)], 8193, "operand stack limit"),
])
def test_hostile_instruction_streams_fail_without_leaks(
    core_executable, tmp_path, instructions, locals_count, error
):
    path = tmp_path / "hostile.apb"
    path.write_bytes(apb(instructions, locals_count=locals_count))
    result = subprocess.run([str(core_executable), str(path)],
                            capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert result.returncode == 1, result.stderr
    assert error in result.stderr


def test_bytecode_roundtrip_and_loader_allocation_failures(
    core_executable, aphelion_executable, tmp_path
):
    source = write_program(tmp_path, "roundtrip.agne", """
        orbit text_copy(v: text) -> text { yield v + "\\0tail" }
        reveal(text_copy("start"))
        reveal(-9223372036854775808)
        reveal(to_decimal(5))
    """)
    compiled = run_cli(aphelion_executable, "compile", str(source))
    assert compiled.returncode == 0, compiled.stderr
    bytecode = source.with_suffix(".apb")
    rewritten = tmp_path / "rewritten.apb"
    result = subprocess.run([str(core_executable), str(bytecode), str(rewritten)],
                            capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "start\0tail\n-9223372036854775808\n5\n"
    assert bytecode.read_bytes() == rewritten.read_bytes()
    allocation = subprocess.run([str(core_executable), "--alloc-read", str(bytecode)],
                                capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert allocation.returncode == 0, allocation.stderr
    assert "loader allocation checks passed" in allocation.stdout
