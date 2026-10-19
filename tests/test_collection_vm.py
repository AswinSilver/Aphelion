"""Primitive list API, hostile bytecode, and retained-allocation checks."""

import struct
import subprocess

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)
from test_reliability import apb


CONSTANT, POP, DUP, LOAD_LOCAL, STORE_LOCAL = 1, 4, 5, 6, 7
LOAD_GLOBAL, STORE_GLOBAL, MAKE_FUNCTION = 8, 9, 10
EQUAL, CALL, RETURN, REVEAL, TO_TEXT = 18, 29, 30, 31, 32
LENGTH, KIND, LIST, INDEX = 40, 41, 43, 44


def whole(value):
    return b"\x01" + struct.pack("<q", value)


def text(value):
    return b"\x04" + struct.pack("<I", len(value)) + value


@pytest.fixture(scope="session")
def collection_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("collection-vm")
    support = ROOT / "tests" / "support"
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("file", "value", "bytecode", "vm"):
        output = build / (name + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(ROOT / "src" / (name + ".c")), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("collection-checks")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"),
                             str(support / "collection_checks.c"), *math_libraries(),
                             "-o", str(executable)], cwd=ROOT, capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def run_bytecode(collection_executable, tmp_path, instructions, **sections):
    path = tmp_path / "collection.apb"
    path.write_bytes(apb(instructions, **sections))
    result = subprocess.run([str(collection_executable), str(path)], capture_output=True,
                            text=True, encoding="utf-8", timeout=5)
    assert result.returncode in (0, 1), result.stderr
    return result


def test_list_constructor_ownership_equality_escaping_and_oom(collection_executable):
    result = subprocess.run([str(collection_executable), "--values"], capture_output=True,
                            text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert "value allocation cleanup checks passed" in result.stdout


@pytest.mark.parametrize(("tag", "constants", "expected"), [
    (1, whole(-7) + whole(42), "[-7, 42]"),
    (2, b"\x02" + struct.pack("<d", 1.5) + b"\x02" + struct.pack("<d", -2.25), "[1.5, -2.25]"),
    (3, b"\x03\x01\x03\x00", "[bright, dim]"),
    (4, text(b'a\0"\\\n\r\t\x01\x7f') + text("é🌙".encode()),
     '["a\\0\\"\\\\\\n\\r\\t\\x01\\x7f", "é🌙"]'),
    (5, b"\x05" + struct.pack("<I", 65) + b"\x05" + struct.pack("<I", 0x1f319), "[U+0041, U+1F319]"),
])
def test_lists_print_convert_measure_and_report_kind(collection_executable, tmp_path, tag, constants, expected):
    instructions = [(CONSTANT, 0, 0), (CONSTANT, 1, 0), (LIST, 2, tag),
                    (DUP, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
                    (DUP, 0, 0), (TO_TEXT, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
                    (DUP, 0, 0), (LENGTH, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
                    (KIND, 0, 0), (REVEAL, 0, 0), (0, 0, 0)]
    result = run_bytecode(collection_executable, tmp_path, instructions, constants=constants, count=2)
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{expected}\n{expected}\n2\nlist\n"


@pytest.mark.parametrize("tag", range(6))
def test_typed_and_untyped_empty_sequences_equal(collection_executable, tmp_path, tag):
    result = run_bytecode(collection_executable, tmp_path, [
        (LIST, 0, tag), (DUP, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
        (DUP, 0, 0), (LENGTH, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
        (LIST, 0, 0), (EQUAL, 0, 0), (REVEAL, 0, 0), (0, 0, 0)])
    assert result.returncode == 0, result.stderr
    assert result.stdout == "[]\n0\nbright\n"


@pytest.mark.parametrize(("left", "right", "expected"), [
    ([1, 2], [1, 2], "bright"), ([1, 2], [2, 1], "dim"),
    ([1, 2], [1], "dim"), ([9007199254740992], [9007199254740993], "dim"),
])
def test_equality_compares_order_length_and_exact_wholes(collection_executable, tmp_path, left, right, expected):
    values = left + right
    instructions = [(CONSTANT, i, 0) for i in range(len(left))] + [(LIST, len(left), 1)]
    instructions += [(CONSTANT, len(left) + i, 0) for i in range(len(right))]
    instructions += [(LIST, len(right), 1), (EQUAL, 0, 0), (REVEAL, 0, 0), (0, 0, 0)]
    result = run_bytecode(collection_executable, tmp_path, instructions,
                          constants=b"".join(whole(n) for n in values), count=len(values))
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected + "\n"


def test_index_retains_text_after_list_is_consumed(collection_executable, tmp_path):
    result = run_bytecode(collection_executable, tmp_path, [
        (CONSTANT, 0, 0), (LIST, 1, 4), (CONSTANT, 1, 0), (INDEX, 0, 0),
        (REVEAL, 0, 0), (0, 0, 0)], constants=text(b"a\0tail") + whole(0), count=2)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "a\0tail\n"


@pytest.mark.parametrize(("tag", "constant", "expected"), [
    (1, whole(-17), "-17"), (2, b"\x02" + struct.pack("<d", 0.25), "0.25"),
    (3, b"\x03\x00", "dim"), (5, b"\x05" + struct.pack("<I", 0x1f319), "U+1F319"),
])
@pytest.mark.parametrize("index", [0, 1])
def test_index_returns_each_primitive_at_first_and_last_position(collection_executable, tmp_path, tag, constant, expected, index):
    result = run_bytecode(collection_executable, tmp_path, [
        (CONSTANT, 0, 0), (CONSTANT, 0, 0), (LIST, 2, tag),
        (CONSTANT, 1, 0), (INDEX, 0, 0), (REVEAL, 0, 0), (0, 0, 0)],
        constants=constant + whole(index), count=2)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected + "\n"


@pytest.mark.parametrize("index", [-9223372036854775808, -1, 1, 9223372036854775807])
def test_index_signed_bounds(collection_executable, tmp_path, index):
    result = run_bytecode(collection_executable, tmp_path, [
        (CONSTANT, 0, 0), (LIST, 1, 4), (CONSTANT, 1, 0), (INDEX, 0, 0), (0, 0, 0)],
        constants=text(b"owned") + whole(index), count=2)
    assert result.returncode == 1
    assert "out of bounds" in result.stderr


@pytest.mark.parametrize("tag", [0, 1, 4])
def test_empty_index_is_out_of_bounds(collection_executable, tmp_path, tag):
    result = run_bytecode(collection_executable, tmp_path,
                          [(LIST, 0, tag), (CONSTANT, 0, 0), (INDEX, 0, 0), (0, 0, 0)],
                          constants=whole(0), count=1)
    assert result.returncode == 1
    assert "out of bounds" in result.stderr


@pytest.mark.parametrize("constant", [b"\x00", b"\x02" + struct.pack("<d", 0),
                                       b"\x03\x01", text(b"0"), b"\x05" + struct.pack("<I", 65), b"\x07"])
def test_index_rejects_nonwhole_index(collection_executable, tmp_path, constant):
    result = run_bytecode(collection_executable, tmp_path,
                          [(LIST, 0, 4), (CONSTANT, 0, 0), (INDEX, 0, 0), (0, 0, 0)],
                          constants=constant, count=1)
    assert result.returncode == 1
    assert "index requires whole" in result.stderr


@pytest.mark.parametrize("target", [b"\x00", whole(3), text(b"owned"), b"\x07"])
def test_index_rejects_nonlist_target(collection_executable, tmp_path, target):
    result = run_bytecode(collection_executable, tmp_path,
                          [(CONSTANT, 0, 0), (CONSTANT, 1, 0), (INDEX, 0, 0), (0, 0, 0)],
                          constants=target + whole(0), count=2)
    assert result.returncode == 1
    assert "list target" in result.stderr


@pytest.mark.parametrize(("instructions", "constants", "count"), [
    ([(CONSTANT, 0, 0), (LIST, 1, 2)], whole(1), 1),
    ([(CONSTANT, 0, 0), (CONSTANT, 1, 0), (LIST, 2, 1)], whole(1) + text(b"owned"), 2),
    ([(LIST, 0, 1), (LIST, 1, 1)], b"", 0),
    ([(2, 0, 0), (LIST, 1, 1)], b"", 0),
    ([(3, 0, 0), (LIST, 1, 1)], b"", 0),
    ([(MAKE_FUNCTION, 0, 0), (LIST, 1, 1)], b"", 0),
])
def test_list_rejects_mixed_nested_and_nonprimitive_values(collection_executable, tmp_path, instructions, constants, count):
    result = run_bytecode(collection_executable, tmp_path, instructions + [(0, 0, 0)],
                          constants=constants, count=count)
    assert result.returncode == 1
    assert "homogeneous" in result.stderr


@pytest.mark.parametrize(("opcode", "operand", "operand2"), [
    (LIST, -1, 1), (LIST, -2147483648, 1), (LIST, 1, 0),
    *((LIST, 0, tag) for tag in [-2147483648, -1, 6, 7, 8, 9, 2147483647]),
    (INDEX, 1, 0), (INDEX, -1, 0), (INDEX, 0, 1), (INDEX, 0, -1),
])
def test_malformed_list_operands_rejected(collection_executable, tmp_path, opcode, operand, operand2):
    result = run_bytecode(collection_executable, tmp_path, [(opcode, operand, operand2), (0, 0, 0)])
    assert result.returncode == 1
    assert "operand" in result.stderr


@pytest.mark.parametrize("opcode", [45, 255])
def test_unknown_opcode_past_index_rejected(collection_executable, tmp_path, opcode):
    result = run_bytecode(collection_executable, tmp_path, [(opcode, 0, 0), (0, 0, 0)])
    assert result.returncode == 1
    assert "opcode" in result.stderr


@pytest.mark.parametrize("opcode", [11, 12, 13, 14, 15, 20, 21, 22, 23])
def test_list_arithmetic_and_ordered_comparison_rejected(collection_executable, tmp_path, opcode):
    result = run_bytecode(collection_executable, tmp_path,
                          [(LIST, 0, 1), (LIST, 0, 1), (opcode, 0, 0), (0, 0, 0)])
    assert result.returncode == 1
    assert "numeric" in result.stderr or "cannot be compared" in result.stderr


@pytest.mark.parametrize("locals_count", [0, 2])
@pytest.mark.parametrize("instructions", [
    [(LIST, 1, 4)], [(LIST, 2147483647, 1)], [(INDEX, 0, 0)],
    [(CONSTANT, 0, 0), (LIST, 2, 4)], [(CONSTANT, 0, 0), (INDEX, 0, 0)],
])
def test_list_underflow_never_consumes_frame_locals(collection_executable, tmp_path, locals_count, instructions):
    result = run_bytecode(collection_executable, tmp_path, instructions + [(0, 0, 0)],
                          constants=text(b"owned partial operand"), count=1, locals_count=locals_count)
    assert result.returncode == 1
    assert "underflow" in result.stderr


@pytest.mark.parametrize("tag", [8, 9, 255])
def test_serialized_collection_and_sentinel_constants_rejected(collection_executable, tmp_path, tag):
    result = run_bytecode(collection_executable, tmp_path, [(0, 0, 0)], constants=bytes([tag]), count=1)
    assert result.returncode == 1
    assert "constant" in result.stderr


def test_writer_and_validator_reject_in_memory_list_sentinel_and_invalid_constants(collection_executable, tmp_path):
    result = subprocess.run([str(collection_executable), "--invalid-constants", str(tmp_path / "invalid.apb")],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr


def lifecycle_instructions():
    return [
        (CONSTANT, 0, 0), (CONSTANT, 1, 0), (LIST, 2, 4), (STORE_LOCAL, 0, 0),
        (LOAD_LOCAL, 0, 0), (STORE_GLOBAL, 0, 0),
        (CONSTANT, 1, 0), (LIST, 1, 4), (STORE_LOCAL, 0, 0),
        (LOAD_GLOBAL, 0, 0), (CONSTANT, 2, 0), (INDEX, 0, 0),
        (REVEAL, 0, 0), (POP, 0, 0),
        (LOAD_GLOBAL, 0, 0), (TO_TEXT, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
        (LIST, 0, 4), (KIND, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
        (LOAD_LOCAL, 0, 0), (LENGTH, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
        (LIST, 0, 0), (STORE_GLOBAL, 0, 0), (0, 0, 0),
    ]


def call_program(callee, *, scalar_argument=False):
    """A caller retains a separate operand below a callee with one local."""
    caller = [(LIST, 0, 1), (MAKE_FUNCTION, 1, 0), (CONSTANT, 0, 0)]
    if not scalar_argument:
        caller += [(LIST, 1, 4)]
    caller += [(CALL, 1, 0), (DUP, 0, 0), (REVEAL, 0, 0), (POP, 0, 0),
               (CONSTANT, 1, 0), (INDEX, 0, 0), (REVEAL, 0, 0), (0, 0, 0)]
    data = bytearray(apb(caller, constants=text(b"arg\0tail") + whole(0), count=2))
    struct.pack_into("<I", data, 16, 2)  # second function, same version-one layout
    name = b"callee"
    data += struct.pack("<I", len(name)) + name + struct.pack("<3I", 1, 1, len(callee))
    data += b"".join(struct.pack("<BiiII", *ins, 2, 1) for ins in callee)
    return data


@pytest.mark.parametrize("scalar_argument", [False, True])
def test_function_argument_return_and_local_cleanup_preserve_list(collection_executable, tmp_path, scalar_argument):
    callee = [(LOAD_LOCAL, 0, 0)]
    if scalar_argument:
        callee += [(LIST, 1, 4)]
    callee += [(RETURN, 0, 0)]
    path = tmp_path / "call.apb"
    path.write_bytes(call_program(callee, scalar_argument=scalar_argument))
    result = subprocess.run([str(collection_executable), str(path)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout == '["arg\\0tail"]\narg\0tail\n'
    sweep = subprocess.run([str(collection_executable), "--sweep", str(path)], capture_output=True, text=True, timeout=10)
    assert sweep.returncode == 0, sweep.stderr


@pytest.mark.parametrize("callee", [
    [(LIST, 1, 4)], [(INDEX, 0, 0)], [(LOAD_LOCAL, 0, 0), (INDEX, 0, 0)],
])
def test_list_ops_respect_nested_call_floor(collection_executable, tmp_path, callee):
    path = tmp_path / "call-underflow.apb"
    path.write_bytes(call_program(callee + [(RETURN, 0, 0)]))
    result = subprocess.run([str(collection_executable), str(path)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1
    assert "underflow" in result.stderr


def test_list_allocations_obey_stack_and_instruction_limits(collection_executable, tmp_path):
    # Empty lists still require an operand slot; successful construction at the
    # limit must be released when pushing fails.
    result = run_bytecode(collection_executable, tmp_path,
                          [(LIST, 0, 0), (0, 0, 0)], locals_count=8192)
    assert result.returncode == 1
    assert "stack limit" in result.stderr
    result = run_bytecode(collection_executable, tmp_path,
                          [(LIST, 0, 0), (POP, 0, 0), (26, 0, 0)])
    assert result.returncode == 1
    assert "instruction limit" in result.stderr


def test_version_one_serialization_reexecution_and_owned_rebindings(collection_executable, tmp_path):
    original = tmp_path / "original.apb"
    rewritten = tmp_path / "rewritten.apb"
    original.write_bytes(apb(lifecycle_instructions(), constants=text(b"first\0tail") + text(b"second") + whole(0),
                             count=3, locals_count=1, globals_=(b"shared",)))
    result = subprocess.run([str(collection_executable), str(original), str(rewritten)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    expected = 'first\0tail\n["first\\0tail", "second"]\nlist\n1\n'
    assert result.stdout == expected
    assert original.read_bytes() == rewritten.read_bytes()
    rerun = subprocess.run([str(collection_executable), str(rewritten)], capture_output=True, text=True, timeout=5)
    assert rerun.returncode == 0, rerun.stderr
    assert rerun.stdout == expected


@pytest.mark.parametrize("failure_tail", [[], [(CONSTANT, 2, 0), (LIST, 1, 4)],
                                          [(LIST, 0, 4), (CONSTANT, 2, 0), (INDEX, 0, 0)],
                                          [(LIST, 0, 4), (CONSTANT, 1, 0), (INDEX, 0, 0)]])
def test_allocation_failure_sweeps_release_lists_and_text(collection_executable, tmp_path, failure_tail):
    path = tmp_path / "sweep.apb"
    # Long text forces several representation-buffer growth allocations.
    instructions = lifecycle_instructions()[:-1] + failure_tail + [(0, 0, 0)]
    path.write_bytes(apb(instructions, constants=text(b"a\0" * 80) + text(b"second") + whole(0),
                         count=3, locals_count=1, globals_=(b"shared",)))
    result = subprocess.run([str(collection_executable), "--sweep", str(path)], capture_output=True,
                            text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert "VM allocation cleanup checks passed" in result.stdout
