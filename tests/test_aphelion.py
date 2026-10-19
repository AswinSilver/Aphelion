"""End-to-end tests for the Aphelion compiler and bytecode VM.

The tests build the C program from the checked-in sources, then exercise the
same command-line interface a user runs.  This keeps the test suite independent
of a pre-existing executable and verifies the lexer, parser, semantic checker,
compiler, bytecode file format, and VM together.
"""

from __future__ import annotations

import subprocess
import struct
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "include"
SOURCES = sorted((ROOT / "src").glob("*.c"))


def run_cli(
    executable: Path,
    *args: str,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(executable), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        input=input_text,
        check=False,
        timeout=5,
    )


def write_program(tmp_path: Path, name: str, source: str) -> Path:
    path = tmp_path / name
    path.write_text(source, encoding="utf-8")
    return path


def test_help_and_version(aphelion_executable: Path) -> None:
    help_result = run_cli(aphelion_executable, "--help")
    version_result = run_cli(aphelion_executable, "--version")

    assert help_result.returncode == 0
    assert "aphelion run" in help_result.stdout
    assert version_result.returncode == 0
    assert version_result.stdout.startswith("aphelion ")


def test_lexer_tokens_and_comments(aphelion_executable: Path, tmp_path: Path) -> None:
    source = write_program(
        tmp_path,
        "tokens.agne",
        "/* block */ forge value: whole = 12 + 3.5 // trailing\n",
    )

    result = run_cli(aphelion_executable, "tokens", str(source))

    assert result.returncode == 0, result.stderr
    assert "forge" in result.stdout
    assert "whole" in result.stdout
    assert "decimal" in result.stdout
    assert "+" in result.stdout
    assert "<eof>" in result.stdout


def test_arithmetic_text_and_typed_variables(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "values.agne",
        """
        forge left: whole = 2
        shift right: whole = 3
        forge label: text = "sum: "
        reveal(1 + left * right)
        reveal(label + "ok")
        reveal(bright and not dim)
        reveal(hollow)
        reveal(to_text(left))
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["7", "sum: ok", "bright", "hollow", "2"]


def test_reveal_formats_decimal_glyph_and_list_values(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "reveal_values.agne",
        "reveal(1.25)\nreveal('A')\nreveal([1, 2])\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1.25", "U+0041", "[1, 2]"]


def test_text_and_glyph_ordered_comparisons_are_length_aware(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "ordered_text.agne",
        'reveal("a\\0b" < "a\\0c")\n'
        'reveal("a\\0b" > "a\\0")\n'
        "reveal('A' < 'B')\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["bright", "bright", "bright"]


def test_when_cycle_escape_and_skip(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "flow.agne",
        """
        shift count: whole = 0
        cycle count < 8 {
            count += 1
            when count == 3 {
                skip
            }
            when count == 7 {
                escape
            }
            reveal(count)
        }
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "2", "4", "5", "6"]


def test_recursive_orbit(aphelion_executable: Path, tmp_path: Path) -> None:
    source = write_program(
        tmp_path,
        "factorial.agne",
        """
        orbit factorial(value: whole) -> whole {
            when value <= 1 {
                yield 1
            } otherwise {
                yield value * factorial(value - 1)
            }
        }
        reveal(factorial(6))
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout == "720\n"


def test_check_does_not_execute_program(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(tmp_path, "check.agne", 'reveal("must not run")\n')

    result = run_cli(aphelion_executable, "check", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{source}: valid\n"


def test_to_text_conversion(aphelion_executable: Path, tmp_path: Path) -> None:
    source = write_program(
        tmp_path,
        "conversion.agne",
        "reveal(\"score: \" + to_text(42))\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout == "score: 42\n"


def test_receive_reads_one_line_without_the_newline(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "receive.agne",
        "forge value: text = receive()\nreveal(value)\n",
    )

    result = run_cli(aphelion_executable, "run", str(source), input_text="hello\n")

    assert result.returncode == 0, result.stderr
    assert result.stdout == "hello\n"


@pytest.mark.parametrize("input_text", ["\n", ""])
def test_receive_empty_line_and_eof_return_empty_text(
    aphelion_executable: Path, tmp_path: Path, input_text: str
) -> None:
    source = write_program(tmp_path, "receive_empty.agne", "reveal(receive())\n")

    result = run_cli(aphelion_executable, "run", str(source), input_text=input_text)

    assert result.returncode == 0, result.stderr
    assert result.stdout == "\n"


def test_receive_handles_long_lines_without_truncation(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(tmp_path, "receive_long.agne", "reveal(receive())\n")
    value = "x" * 5000

    result = run_cli(aphelion_executable, "run", str(source), input_text=value + "\n")

    assert result.returncode == 0, result.stderr
    assert result.stdout == value + "\n"


def test_numeric_conversions_have_defined_runtime_values(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "conversions.agne",
        """
        reveal(to_whole("42"))
        reveal(to_decimal("3.5"))
        reveal(to_text(bright))
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["42", "3.5", "bright"]


@pytest.mark.parametrize(
    "expression",
    ['to_whole("4x")', 'to_whole(" 4")', 'to_decimal("nan")'],
)
def test_invalid_numeric_conversion_is_a_runtime_error(
    aphelion_executable: Path, tmp_path: Path, expression: str
) -> None:
    source = write_program(tmp_path, "bad_conversion.agne", f"reveal({expression})\n")

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode != 0
    assert "cannot convert" in result.stderr.lower()
    assert f"{source}:" in result.stderr


def test_numeric_conversion_overflow_is_rejected(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "conversion_overflow.agne",
        "reveal(to_whole(9223372036854775808.0))\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode != 0
    assert "out of whole range" in result.stderr.lower()


def test_whole_division_preserves_the_static_type(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "whole_division.agne",
        "forge result: whole = 5 / 2\nreveal(result)\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout == "2\n"


def test_newline_after_operator_continues_expression(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "multiline_expression.agne",
        "forge value: whole = 1 +\n2 *\n3\nreveal(value)\n",
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout == "7\n"


def test_nested_shadowing_uses_the_semantic_binding(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "shadowing.agne",
        """
        orbit scope() -> whole {
            forge value: whole = 1
            when bright {
                forge value: whole = 2
                reveal(value)
            }
            yield value
        }
        reveal(scope())
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["2", "1"]


def test_global_reference_is_not_captured_by_a_later_local(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "global_before_local.agne",
        """
        forge value: whole = 10
        orbit scope() -> whole {
            reveal(value)
            forge value: whole = 20
            yield value
        }
        reveal(scope())
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["10", "20"]


def test_top_level_block_shadowing_preserves_global_binding(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "top_level_shadowing.agne",
        """
        forge value: whole = 1
        {
            forge value: whole = 2
            reveal(value)
        }
        reveal(value)
        """,
    )

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["2", "1"]


def test_missing_function_return_is_a_semantic_error(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "missing_return.agne",
        "orbit missing() -> whole { reveal(1) }\n",
    )

    result = run_cli(aphelion_executable, "check", str(source))

    assert result.returncode != 0
    assert "does not yield" in result.stderr.lower()
    assert f"{source}:" in result.stderr


def test_function_argument_type_mismatch_is_a_semantic_error(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "bad_argument.agne",
        """
        orbit takes(value: whole) -> whole {
            yield value
        }
        reveal(takes("wrong"))
        """,
    )

    result = run_cli(aphelion_executable, "check", str(source))

    assert result.returncode != 0
    assert "argument" in result.stderr.lower()
    assert "expected whole" in result.stderr.lower()


def test_compile_and_disassemble_bytecode(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(
        tmp_path,
        "compiled.agne",
        """
        reveal(40 + 2)
        reveal(to_whole("3"))
        reveal(to_decimal("4.5"))
        reveal(receive())
        """,
    )

    compile_result = run_cli(aphelion_executable, "compile", str(source))
    bytecode = source.with_suffix(".apb")
    disasm_result = run_cli(aphelion_executable, "disasm", str(bytecode))

    assert compile_result.returncode == 0, compile_result.stderr
    assert bytecode.exists()
    assert disasm_result.returncode == 0, disasm_result.stderr
    assert "APB" not in disasm_result.stdout  # header is consumed by the reader
    assert "CONSTANT" in disasm_result.stdout
    assert "REVEAL" in disasm_result.stdout
    assert "RECEIVE" in disasm_result.stdout
    assert "TO_WHOLE" in disasm_result.stdout
    assert "TO_DECIMAL" in disasm_result.stdout


def test_disasm_rejects_trailing_corrupt_bytecode(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    source = write_program(tmp_path, "corrupt.agne", "reveal(1)\n")
    compile_result = run_cli(aphelion_executable, "compile", str(source))
    bytecode = source.with_suffix(".apb")
    bytecode.write_bytes(bytecode.read_bytes() + b"corrupt")

    result = run_cli(aphelion_executable, "disasm", str(bytecode))

    assert compile_result.returncode == 0, compile_result.stderr
    assert result.returncode != 0
    assert "trailing" in result.stderr.lower()


def test_disasm_rejects_invalid_bytecode_header(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    bytecode = tmp_path / "invalid.apb"
    bytecode.write_bytes(b"not-aphelion")

    result = run_cli(aphelion_executable, "disasm", str(bytecode))

    assert result.returncode != 0
    assert "invalid" in result.stderr.lower()


def test_disasm_rejects_unknown_bytecode_opcode(
    aphelion_executable: Path, tmp_path: Path
) -> None:
    bytecode = tmp_path / "unknown_opcode.apb"
    bytecode.write_bytes(
        b"APB1"
        + struct.pack("<6I", 1, 0, 0, 1, 0, 0)
        + struct.pack("<I", 4)
        + b"main"
        + struct.pack("<3I", 0, 0, 1)
        + struct.pack("<BiiII", 255, 0, 0, 1, 1)
    )

    result = run_cli(aphelion_executable, "disasm", str(bytecode))

    assert result.returncode != 0
    assert "opcode" in result.stderr.lower()


@pytest.mark.parametrize(
    ("source_text", "expected"),
    [
        ("reveal(missing_name)\n", "undefined name"),
        ("forge value: whole = 1\nvalue = 2\n", "immutable"),
        ("reveal(10 / 0)\n", "division by zero"),
        ("forge value = ;\n", "expected an initializer"),
        ("reveal(\"unterminated)\n", "unterminated"),
    ],
)
def test_invalid_programs_have_location_bearing_diagnostics(
    aphelion_executable: Path,
    tmp_path: Path,
    source_text: str,
    expected: str,
) -> None:
    source = write_program(tmp_path, "invalid.agne", source_text)

    result = run_cli(aphelion_executable, "run", str(source))
    diagnostic = result.stderr.lower()

    assert result.returncode != 0
    assert "traceback" not in diagnostic
    assert f"{source}:" in result.stderr
    assert expected in diagnostic


def test_extension_is_required(aphelion_executable: Path, tmp_path: Path) -> None:
    source = write_program(tmp_path, "wrong.txt", "reveal(1)\n")

    result = run_cli(aphelion_executable, "run", str(source))

    assert result.returncode != 0
    assert ".agne" in result.stderr
