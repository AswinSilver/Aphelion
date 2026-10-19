"""Builtin opcodes exercised independently of frontend arity/type checking."""

import struct
import subprocess

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)
from test_core_checks import core_executable  # shared leak-tracking C driver fixture
from test_reliability import apb


LENGTH, KIND, ASSERT = 40, 41, 42
CONSTANT, HOLLOW, SILENT, MAKE_FUNCTION, REVEAL = 1, 2, 3, 10, 31


def text_constant(value):
    return b"\x04" + struct.pack("<I", len(value)) + value


def run_bytecode(core_executable, tmp_path, instructions, **sections):
    path = tmp_path / "builtin.apb"
    path.write_bytes(apb(instructions, **sections))
    result = subprocess.run([str(core_executable), str(path)], capture_output=True,
                            text=True, encoding="utf-8", timeout=5)
    # The driver's status 2 specifically signals leaked allocations.
    assert result.returncode in (0, 1), result.stderr
    return result


@pytest.mark.parametrize("value", [b"", b"abc", b"a\0b", "é🌙".encode("utf-8")])
def test_length_counts_bytes_including_embedded_nul(core_executable, tmp_path, value):
    result = run_bytecode(core_executable, tmp_path,
                          [(CONSTANT, 0, 0), (LENGTH, 0, 0), (REVEAL, 0, 0), (0, 0, 0)],
                          constants=text_constant(value), count=1)
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{len(value)}\n"


@pytest.mark.parametrize(("constant", "expected"), [
    (b"\x00", "hollow"),
    (b"\x01" + struct.pack("<q", 7), "whole"),
    (b"\x02" + struct.pack("<d", 1.5), "decimal"),
    (b"\x03\x01", "signal"),
    (text_constant(b"a\0b"), "text"),
    (b"\x05" + struct.pack("<I", 0x1f319), "glyph"),
    (b"\x06" + struct.pack("<I", 0), "function"),
    (b"\x07", "silent"),
])
def test_kind_stable_names_for_every_value(core_executable, tmp_path, constant, expected):
    result = run_bytecode(core_executable, tmp_path,
                          [(CONSTANT, 0, 0), (KIND, 0, 0), (REVEAL, 0, 0), (0, 0, 0)],
                          constants=constant, count=1)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected + "\n"


@pytest.mark.parametrize(("producer", "expected"), [
    (HOLLOW, "hollow"), (SILENT, "silent"), (MAKE_FUNCTION, "function"),
])
def test_kind_accepts_vm_created_values(core_executable, tmp_path, producer, expected):
    result = run_bytecode(core_executable, tmp_path,
                          [(producer, 0, 0), (KIND, 0, 0), (REVEAL, 0, 0), (0, 0, 0)])
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected + "\n"


@pytest.mark.parametrize("arity", [1, 2])
def test_bright_assert_returns_silent(core_executable, tmp_path, arity):
    instructions = [(CONSTANT, 0, 0)]
    if arity == 2:
        instructions.append((CONSTANT, 1, 0))
    instructions += [(ASSERT, arity, 0), (KIND, 0, 0), (REVEAL, 0, 0), (0, 0, 0)]
    result = run_bytecode(core_executable, tmp_path, instructions,
                          constants=b"\x03\x01" + text_constant(b"ignored\0message"), count=2)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "silent\n"


@pytest.mark.parametrize("message", [None, b"", b"before\0after", b"line\nnext\r\t\\", b"x" * 4096])
def test_dim_assert_reports_bounded_escaped_message(core_executable, tmp_path, message):
    instructions = [(CONSTANT, 0, 0)]
    constants = b"\x03\x00"
    if message is not None:
        instructions.append((CONSTANT, 1, 0))
        constants += text_constant(message)
    instructions += [(ASSERT, 1 if message is None else 2, 0), (0, 0, 0)]
    result = run_bytecode(core_executable, tmp_path, instructions,
                          constants=constants, count=1 if message is None else 2)
    assert result.returncode == 1
    assert "assertion failed" in result.stderr
    assert "\0" not in result.stderr
    assert len(result.stderr) <= 256
    if message == b"before\0after":
        assert "before\\0after" in result.stderr
    if message == b"line\nnext\r\t\\":
        assert "line\\nnext\\r\\t\\\\" in result.stderr
    if message is not None and len(message) > 256:
        assert "..." in result.stderr


@pytest.mark.parametrize("constant", [b"\x00", b"\x01" + struct.pack("<q", 7),
                                       b"\x03\x01", b"\x07"])
def test_length_rejects_nontext_tags(core_executable, tmp_path, constant):
    result = run_bytecode(core_executable, tmp_path,
                          [(CONSTANT, 0, 0), (LENGTH, 0, 0), (0, 0, 0)],
                          constants=constant, count=1)
    assert result.returncode == 1
    assert "length requires text" in result.stderr


@pytest.mark.parametrize("arity", [1, 2])
def test_assert_rejects_nonsignal_even_with_owned_message(core_executable, tmp_path, arity):
    instructions = [(CONSTANT, 0, 0)]
    if arity == 2:
        instructions.append((CONSTANT, 1, 0))
    instructions += [(ASSERT, arity, 0), (0, 0, 0)]
    result = run_bytecode(core_executable, tmp_path, instructions,
                          constants=text_constant(b"not a signal") + text_constant(b"message"), count=2)
    assert result.returncode == 1
    assert "assert requires a signal" in result.stderr


@pytest.mark.parametrize("signal", [0, 1])
@pytest.mark.parametrize("message", [b"\x00", b"\x01" + struct.pack("<q", 7), b"\x07"])
def test_assert_checks_message_tag_even_when_bright(core_executable, tmp_path, signal, message):
    result = run_bytecode(core_executable, tmp_path,
                          [(CONSTANT, 0, 0), (CONSTANT, 1, 0), (ASSERT, 2, 0), (0, 0, 0)],
                          constants=bytes([3, signal]) + message, count=2)
    assert result.returncode == 1
    assert "assert message requires text" in result.stderr


@pytest.mark.parametrize(("opcode", "arity"), [(LENGTH, 0), (KIND, 0), (ASSERT, 1), (ASSERT, 2)])
@pytest.mark.parametrize("locals_count", [0, 1])
def test_builtin_underflow_cannot_consume_locals(core_executable, tmp_path, opcode, arity, locals_count):
    result = run_bytecode(core_executable, tmp_path, [(opcode, arity, 0), (0, 0, 0)],
                          locals_count=locals_count)
    assert result.returncode == 1
    assert "underflow" in result.stderr


def test_assert_partial_underflow_releases_owned_message(core_executable, tmp_path):
    result = run_bytecode(core_executable, tmp_path,
                          [(CONSTANT, 0, 0), (ASSERT, 2, 0), (0, 0, 0)],
                          constants=text_constant(b"owned message"), count=1, locals_count=1)
    assert result.returncode == 1
    assert "underflow" in result.stderr


@pytest.mark.parametrize("instructions", [
    [(6, 0, 0), (KIND, 0, 0), (0, 0, 0)],
    [(8, 0, 0), (KIND, 0, 0), (0, 0, 0)],
])
def test_kind_never_exposes_uninitialized_sentinel(core_executable, tmp_path, instructions):
    result = run_bytecode(core_executable, tmp_path, instructions,
                          locals_count=1, globals_=(b"uninitialized",))
    assert result.returncode == 1
    assert "before initialization" in result.stderr
    assert result.stdout == ""


@pytest.mark.parametrize(("opcode", "operand", "operand2"), [
    (LENGTH, 1, 0), (LENGTH, -1, 0), (LENGTH, 0, 1),
    (KIND, 1, 0), (KIND, -1, 0), (KIND, 0, -1),
    (ASSERT, -2147483648, 0), (ASSERT, -1, 0), (ASSERT, 0, 0),
    (ASSERT, 3, 0), (ASSERT, 2147483647, 0),
    (ASSERT, 1, 1), (ASSERT, 2, -1),
])
def test_malformed_builtin_operands_are_rejected(core_executable, tmp_path, opcode, operand, operand2):
    result = run_bytecode(core_executable, tmp_path, [(opcode, operand, operand2), (0, 0, 0)])
    assert result.returncode == 1
    assert "operand" in result.stderr or "argument count" in result.stderr


def test_builtin_version_one_roundtrip(core_executable, aphelion_executable, tmp_path):
    instructions = [
        (CONSTANT, 0, 0), (LENGTH, 0, 0), (KIND, 0, 0), (REVEAL, 0, 0), (4, 0, 0),
        (CONSTANT, 1, 0), (CONSTANT, 0, 0), (ASSERT, 2, 0), (KIND, 0, 0),
        (REVEAL, 0, 0), (0, 0, 0),
    ]
    original = tmp_path / "builtin.apb"
    rewritten = tmp_path / "rewritten.apb"
    original.write_bytes(apb(instructions, constants=text_constant(b"a\0b") + b"\x03\x01", count=2))
    result = subprocess.run([str(core_executable), str(original), str(rewritten)],
                            capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "whole\nsilent\n"
    assert rewritten.read_bytes() == original.read_bytes()
    disassembled = subprocess.run([str(aphelion_executable), "disasm", str(rewritten)],
                                  capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert disassembled.returncode == 0, disassembled.stderr
    for name in ("LENGTH", "KIND", "ASSERT"):
        assert name in disassembled.stdout


@pytest.fixture(scope="session")
def builtin_allocation_executable(tmp_path_factory):
    """Reuse the allocation ledger to cover runtime-only KIND allocations too."""
    build = tmp_path_factory.mktemp("builtin-allocation")
    support = ROOT / "tests" / "support"
    source = build / "builtin_allocation.c"
    source.write_text(r'''
#include "fail_alloc.h"
#include "aphelion/vm.h"
#include <stdio.h>

int main(int argc, char **argv) {
    size_t point, attempts = 0u;
    if (argc != 2) return 3;
    for (point = 0u; point <= attempts; ++point) {
        AphelionBytecode bytecode;
        AphelionVM vm;
        char error[256] = {0};
        FILE *output = tmpfile();
        int loaded, ok = 0;
        if (output == NULL) return 3;
        test_alloc_reset(point);
        aphelion_bytecode_init(&bytecode);
        aphelion_vm_init(&vm);
        aphelion_vm_set_output(&vm, output);
        aphelion_vm_set_instruction_limit(&vm, 1000u);
        loaded = aphelion_bytecode_read_file(&bytecode, argv[1], error, sizeof(error));
        if (loaded) {
            ok = aphelion_vm_run(&vm, &bytecode);
            if (!ok && aphelion_vm_error(&vm)->message[0] == '\0') return 3;
        } else if (error[0] == '\0') return 3;
        if (point == 0u) attempts = test_alloc_attempts();
        else if (ok) {
            fprintf(stderr, "allocation failure %zu was masked\n", point);
            return 3;
        }
        aphelion_vm_destroy(&vm);
        aphelion_bytecode_destroy(&bytecode);
        fclose(output);
        if (test_alloc_live() != 0u) {
            fprintf(stderr, "allocation failure %zu leaked %zu objects\n", point, test_alloc_live());
            return 2;
        }
    }
    puts("builtin allocation cleanup checks passed");
    return 0;
}
''', encoding="utf-8")
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("file", "bytecode", "value", "vm"):
        output = build / (name + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(ROOT / "src" / (name + ".c")), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("builtin-allocation")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"), str(source),
                             *math_libraries(), "-o", str(executable)],
                            cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


@pytest.mark.parametrize("signal", [0, 1])
def test_builtin_allocation_failures_release_owned_values(builtin_allocation_executable, tmp_path, signal):
    path = tmp_path / "allocation.apb"
    instructions = [
        (CONSTANT, 0, 0), (KIND, 0, 0), (4, 0, 0),
        (CONSTANT, 0, 0), (LENGTH, 0, 0), (4, 0, 0),
        (CONSTANT, 1, 0), (CONSTANT, 0, 0), (ASSERT, 2, 0), (4, 0, 0), (0, 0, 0),
    ]
    path.write_bytes(apb(instructions, constants=text_constant(b"owned\0message") + bytes([3, signal]), count=2))
    result = subprocess.run([str(builtin_allocation_executable), str(path)],
                            capture_output=True, text=True, encoding="utf-8", timeout=5)
    assert result.returncode == 0, result.stderr
    assert "builtin allocation cleanup checks passed" in result.stdout
