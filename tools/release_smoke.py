#!/usr/bin/env python3
"""Small, dependency-free release verification of an exact Aphelion binary."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TIMEOUT_SECONDS = 5
EXAMPLES = {
    "hello.agne": "Welcome Aswin\nFinal score: 100\n",
    "factorial.agne": "120\n",
    "control_flow.agne": "1\n2\n4\n5\n6\n7\n",
}


class SmokeFailure(Exception):
    """An observable release contract was not satisfied."""


class Smoke:
    def __init__(self, executable: Path, directory: Path) -> None:
        self.executable = executable
        self.directory = directory
        self.checks = 0

    def run(
        self,
        *args: str,
        status: int = 0,
        stdout: str | None = None,
        contains: tuple[str, ...] = (),
        stderr_contains: tuple[str, ...] = (),
        input_bytes: bytes = b"",
    ) -> None:
        command = [str(self.executable), *args]
        try:
            # Bytes preserve CRLF on stdin, including on native Windows Python.
            result = subprocess.run(
                command, cwd=self.directory, input=input_bytes,
                capture_output=True, shell=False, timeout=TIMEOUT_SECONDS,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SmokeFailure(f"{command!r}: {error}") from error
        try:
            output = result.stdout.decode("utf-8").replace("\r\n", "\n")
            errors = result.stderr.decode("utf-8").replace("\r\n", "\n")
        except UnicodeDecodeError as error:
            raise SmokeFailure(f"{command!r}: output is not UTF-8: {error}") from error
        problems = []
        if result.returncode != status:
            problems.append(f"expected status {status}, got {result.returncode}")
        if stdout is not None and output != stdout:
            problems.append(f"expected stdout {stdout!r}")
        if any(fragment not in output for fragment in contains):
            problems.append(f"stdout must contain {contains!r}")
        if stderr_contains:
            if any(fragment not in errors for fragment in stderr_contains):
                problems.append(f"stderr must contain {stderr_contains!r}")
        elif errors:
            problems.append("expected empty stderr")
        if problems:
            raise SmokeFailure(
                f"{command!r}: {'; '.join(problems)}\n"
                f"stdout: {output!r}\nstderr: {errors!r}"
            )
        self.checks += 1


def smoke(executable: Path) -> int:
    # Even copies of the examples live here: compile writes beside its input.
    with tempfile.TemporaryDirectory(prefix="aphelion release smoke ") as temporary:
        directory = Path(temporary)
        runner = Smoke(executable, directory)
        runner.run("--version", stdout="aphelion 0.1.0\n")
        runner.run("--help", contains=("Usage:", "aphelion run", "aphelion disasm"))

        for name, expected in EXAMPLES.items():
            source = directory / name
            source.write_bytes((ROOT / "examples" / name).read_bytes())
            runner.run("run", str(source), stdout=expected)
            runner.run("check", str(source), stdout=f"{source}: valid\n")

        practical = directory / "builtins_practical.agne"
        practical.write_bytes((ROOT / "examples" / practical.name).read_bytes())
        practical_output = "Hello Ada\n3\nwhole\n7 items\n5\n7\n"
        practical_input = b"Ada\n7\n2.5\n"
        runner.run("run", str(practical), input_bytes=practical_input,
                   stdout=practical_output)
        runner.run("check", str(practical), stdout=f"{practical}: valid\n")
        practical_bytecode = practical.with_suffix(".apb")
        runner.run("compile", str(practical), stdout=f"{practical_bytecode}\n")
        runner.run("disasm", str(practical_bytecode),
                   contains=("RECEIVE", "TO_WHOLE", "TO_DECIMAL", "LENGTH", "KIND", "ASSERT"))

        flow = str(directory / "control_flow.agne")
        runner.run("tokens", flow, contains=("shift", "cycle", "<eof>"))
        runner.run("ast", flow, contains=("program ", "cycle", "name count"))

        source = directory / "stdin conversions.agne"
        source.write_text(
            'forge line: text = receive()\n'
            'reveal(line == "héllo")\n'
            'reveal(line)\n'
            'reveal(to_whole(receive()) + 1)\n'
            'reveal(to_decimal(receive()) + 0.5)\n'
            'reveal(to_text(bright))\n'
            'reveal(receive() == "end\\r")\n'
            'reveal(receive() == "")\n',
            encoding="utf-8",
        )
        runner.run(
            "run", str(source), input_bytes="héllo\r\n41\r\n3.5\r\nend\r".encode("utf-8"),
            stdout="bright\nhéllo\n42\n4\nbright\nbright\nbright\n",
        )
        bytecode = source.with_suffix(".apb")
        runner.run("compile", str(source), stdout=f"{bytecode}\n")
        data = bytecode.read_bytes()
        if len(data) < 28 or data[:8] != b"APB1\x01\x00\x00\x00":
            raise SmokeFailure("compile did not produce an APB1/version-1 bytecode file")
        # disasm loads and validates the bytecode, not just its header.
        runner.run(
            "disasm", str(bytecode),
            contains=("CONSTANT", "REVEAL", "RECEIVE", "TO_WHOLE", "TO_DECIMAL", "TO_TEXT"),
        )
        corrupt = directory / "corrupt.apb"
        corrupt.write_bytes(data + b"trailing garbage")
        runner.run("disasm", str(corrupt), status=1, stdout="", stderr_contains=("trailing",))

        failure = directory / "runtime error.agne"
        failure.write_text("reveal(10 / 0)\n", encoding="utf-8")
        runner.run(
            "run", str(failure), status=1, stdout="",
            stderr_contains=(f"{failure}:", "runtime error:", "division by zero"),
        )
        for args in ((), ("unknown-command",), ("run",)):
            runner.run(*args, status=2, stdout="", stderr_contains=("Usage:",))
        runner.run(
            "repl", input_bytes=b'1 + 2\nforge x = 5; x * 2\n:quit\n',
            stdout="3\n10\n",
        )
        runner.run(
            "repl", input_bytes=b'forge x = 5\nx\n6 * 7', status=1,
            stdout="42\n", stderr_contains=("<repl>:", "undefined name"),
        )
        return runner.checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable", required=True, type=Path,
        help="exact native executable path (no PATH lookup or suffix inference)",
    )
    args = parser.parse_args(argv)
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"executable is not a file: {executable}")
    try:
        checks = smoke(executable)
    except (SmokeFailure, OSError) as error:
        print(f"Release smoke FAILED: {error}", file=sys.stderr)
        return 1
    print(f"Release smoke passed: {checks} checks ({executable})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
