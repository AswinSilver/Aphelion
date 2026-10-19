"""Boundary/ownership regressions, kept separate from the original 33 tests."""

import os
import struct
import subprocess

import pytest

from test_aphelion import run_cli, write_program


@pytest.mark.parametrize(("expression", "expected"), [
    ("-9223372036854775808", "-9223372036854775808"),
    ("9223372036854775807", "9223372036854775807"),
    ("9007199254740992 == 9007199254740993", "dim"),
    ("9223372036854775807 != 9223372036854775806", "bright"),
    ("9007199254740993 > 9007199254740992", "bright"),
    ("-7 / 3", "-2"), ("-7 % 3", "-1"),
    ("7 / -3", "-2"), ("7 % -3", "1"),
    ("to_whole(-9223372036854775808.0)", "-9223372036854775808"),
    ("to_whole(9223372036854774784.0)", "9223372036854774784"),
    ("to_whole(-3.9)", "-3"),
    ('to_whole("-9223372036854775808")', "-9223372036854775808"),
    ("dim and (1 / 0 == 0)", "dim"),
    ("bright or (1 / 0 == 0)", "bright"),
    ("(0.1 + 0.2) > 0.3", "bright"),
    ("to_decimal(9007199254740993) == 9007199254740992.0", "bright"),
])
def test_numeric_and_logical_boundaries(aphelion_executable, tmp_path, expression, expected):
    path = write_program(tmp_path, "boundary.agne", f"reveal({expression})\n")
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected + "\n"


@pytest.mark.parametrize("expression", [
    "9223372036854775807 + 1", "-9223372036854775808 - 1",
    "9223372036854775807 * 2", "3037000500 * 3037000500",
    "-9223372036854775808 * -1", "-(-9223372036854775808)",
    "-9223372036854775808 / -1", "-9223372036854775808 % -1",
    "1 % 0", "1 / 0", "1.0 / 0.0", "1e308 * 2.0",
    "to_whole(9223372036854775808.0)", 'to_whole("-9223372036854775809")',
    'to_decimal("1e9999")', "9223372036854775808",
])
def test_numeric_errors_are_controlled(aphelion_executable, tmp_path, expression):
    path = write_program(tmp_path, "bad_boundary.agne", f"reveal({expression})\n")
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1, result.stderr
    assert f"{path}:" in result.stderr
    assert result.stderr.strip()


def test_widening_happens_at_storage_arguments_and_returns(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "widen.agne", """
        forge a: decimal = 5
        shift b: decimal = 1.0
        b = 5
        orbit half(v: decimal) -> decimal { yield v / 2 }
        orbit five() -> decimal { yield 5 }
        reveal(a / 2)
        reveal(b / 2)
        reveal(half(5))
        reveal(five() / 2)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["2.5"] * 4


def test_receive_crlf_and_final_bare_cr(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "crlf.agne", """
        reveal(receive() == "hello")
        reveal(receive() == "end\\r")
        reveal(receive() == "")
    """)
    result = subprocess.run([str(aphelion_executable), "run", str(path)],
                            input=b"hello\r\nend\r", capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout.decode("utf-8").splitlines() == ["bright"] * 3


def test_binary_text_survives_copy_return_assignment_and_conversion(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "text_lifetime.agne", """
        orbit make(v: text) -> text {
            forge local = v + "\\0b"
            yield local
        }
        shift a = make("a")
        forge old = a
        a = a + "c"
        reveal(old)
        reveal(to_text(a))
        reveal(old == "a\\0b")
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "a\0b\na\0bc\nbright\n"
    debug = run_cli(aphelion_executable, "ast", str(path))
    assert debug.returncode == 0, debug.stderr
    assert "literal \\0b\n" in debug.stdout
    assert "literal a\\0b\n" in debug.stdout
    assert "\0" not in debug.stdout


@pytest.mark.parametrize("source", [
    "reveal(" + "(" * 20000 + "1" + ")" * 20000 + ")",
    "reveal(" + "not " * 10000 + "bright)",
    "reveal(" + "+".join(["1"] * 10000) + ")",
    "{" * 2000 + "reveal(1)" + "}" * 2000,
], ids=["nested-parentheses", "unary-not-chain", "addition-chain", "nested-blocks"])
def test_excessive_syntax_depth_is_a_diagnostic(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "deep.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1, result.stderr[:1000]
    assert "limit exceeded" in result.stderr


@pytest.mark.parametrize("command", ["run", "check", "tokens", "ast", "compile"])
def test_raw_nul_source_is_never_silently_truncated(aphelion_executable, tmp_path, command):
    path = tmp_path / "nul.agne"
    path.write_bytes(b"reveal(1)\n\0garbage")
    result = run_cli(aphelion_executable, command, str(path))
    assert result.returncode == 1
    assert "raw NUL" in result.stderr


def test_recursion_exhaustion_has_a_controlled_error(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "recursion.agne", """
        orbit again(v: text) -> text { yield again(v + "x") }
        reveal(again("a"))
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert "limit exceeded" in result.stderr


@pytest.mark.parametrize("source", [
    "reveal(later)\nforge later: whole = 3\n",
    "shift value: whole = (value = 1)\n",
    "forge v: silent = reveal(1)\n",
    "orbit bad(v: silent) -> silent { yield }\n",
    "forge to_text = 3\n",
])
def test_initialization_and_value_type_errors(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "invalid_semantic.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert result.stderr.strip()


def test_early_function_cannot_read_uninitialized_global(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "early.agne", """
        orbit read() -> whole { yield later }
        reveal(read())
        forge later: whole = 3
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert "before initialization" in result.stderr


@pytest.mark.parametrize("source", [
    "orbit f(v: whole) -> whole { yield v }\nreveal(f())\n",
    "orbit f() -> whole { yield \"bad\" }\n",
    "forge f = 1\nf()\n",
    "reveal(receive(1))\n",
    "reveal(to_whole(bright))\n",
    "forge x = 1\nforge x = 2\n",
    "{ forge inner = 1 }\nreveal(inner)\n",
])
def test_invalid_signatures_returns_and_scopes(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "signature.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert result.stderr.strip()


def test_precedence_associativity_and_assignment(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "precedence.agne", """
        shift a = 0
        shift b = 0
        a = b = 2 + 3 * 4
        reveal(a)
        reveal(b)
        reveal(20 - 5 - 3)
        reveal(not dim and bright or dim)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["14", "14", "12", "bright"]


def test_grouped_subexpression_in_condition(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "condition.agne", """
        shift n = 0
        cycle (n + 1) < 3 { n += 1 }
        when (n + 1) == 3 { reveal(n) }
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "2\n"


@pytest.mark.skipif(os.name == "nt", reason="/dev/full is a Unix error stream")
def test_output_errors_change_exit_status(aphelion_executable, tmp_path):
    if not os.path.exists("/dev/full"):
        pytest.skip("/dev/full unavailable")
    path = write_program(tmp_path, "output.agne", 'reveal("hello")')
    with open("/dev/full", "wb") as output:
        result = subprocess.run([str(aphelion_executable), "run", str(path)],
                                stdout=output, stderr=subprocess.PIPE, timeout=5)
    assert result.returncode == 1
    assert b"output failed" in result.stderr


def apb(instructions, *, constants=b"", count=0, locals_count=0, arity=0, globals_=()):
    def string(b):
        return struct.pack("<I", len(b)) + b
    return (b"APB1" + struct.pack("<5I", 1, 0, count, 1, len(globals_)) + string(b"test")
            + constants + b"".join(string(g) for g in globals_) + string(b"main")
            + struct.pack("<3I", arity, locals_count, len(instructions))
            + b"".join(struct.pack("<BiiII", *ins, 1, 1) for ins in instructions))


@pytest.mark.parametrize("data", [
    apb([(26, 999, 0)]),  # invalid JUMP
    apb([(0, 0, 0)], arity=1, locals_count=1),
    apb([(0, 1, 0)]),  # unused operand must be zero
    apb([(0, 0, 0)], constants=b"\x02" + struct.pack("<d", float("nan")), count=1),
    apb([(0, 0, 0)], constants=b"\x05" + struct.pack("<I", 0xd800), count=1),
    apb([(0, 0, 0)], globals_=(b"x", b"x")),
    apb([(0, 0, 0)], globals_=(b"x\0y",)),
])
def test_malformed_bytecode_rejected(aphelion_executable, tmp_path, data):
    path = tmp_path / "invalid.apb"
    path.write_bytes(data)
    result = run_cli(aphelion_executable, "disasm", str(path))
    assert result.returncode == 1
    assert result.stderr.strip()
