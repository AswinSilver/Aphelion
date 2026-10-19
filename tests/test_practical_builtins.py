"""A small ordinary program using the complete essential builtin set."""

import subprocess

from test_aphelion import ROOT, run_cli
from test_core_checks import core_executable


EXAMPLE = ROOT / "examples" / "builtins_practical.agne"
INPUT = "Ada\n7\n2.5\n"
EXPECTED = "Hello Ada\n3\nwhole\n7 items\n5\n7\n"


def test_practical_builtin_program_runs_with_input(aphelion_executable):
    result = run_cli(aphelion_executable, "run", str(EXAMPLE), input_text=INPUT)

    assert result.returncode == 0, result.stderr
    assert result.stdout == EXPECTED


def test_practical_builtin_program_compiles_and_executes_from_bytecode(
    aphelion_executable, core_executable, tmp_path
):
    source = tmp_path / "practical.agne"
    source.write_bytes(EXAMPLE.read_bytes())
    compiled = run_cli(aphelion_executable, "compile", str(source))
    bytecode = source.with_suffix(".apb")

    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run(
        [str(core_executable), str(bytecode)],
        input=INPUT.encode("utf-8"),
        capture_output=True,
        timeout=5,
    )

    assert result.returncode == 0, result.stderr.decode("utf-8", "replace")
    assert result.stdout.decode("utf-8").replace("\r\n", "\n") == EXPECTED
