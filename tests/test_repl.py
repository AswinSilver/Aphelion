"""One physical line -> fresh parser/semantic/compiler/bytecode/VM execution."""

import os
from pathlib import Path
import subprocess
import time

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      reject_sanitizer_output, test_cflags as extra_cflags)


LIMIT = 64 * 1024


def repl(executable, source=b"", *args):
    if isinstance(source, str):
        source = source.encode("utf-8")
    result = subprocess.run([str(executable), "repl", *args], input=source,
                            capture_output=True, timeout=10, check=False)
    # Preserve binary stdin, but normalize native Windows output line endings.
    result.stdout = result.stdout.decode("utf-8").replace("\r\n", "\n")
    result.stderr = result.stderr.decode("utf-8").replace("\r\n", "\n")
    return result


@pytest.mark.parametrize(("source", "expected"), [
    ("1 + 2 * 3", "7\n"),
    ('"hello " + "world"; bright and not dim; hollow; \'a\'; 1.5',
     "hello world\nbright\nhollow\nU+0061\n1.5\n"),
    ("shift x = 2; x += 3; x", "5\n5\n"),
    ('reveal("once"); assert(bright); orbit quiet() -> silent { yield }; quiet()',
     "once\n"),
    ('length("hé"); kind([1, 2]); to_whole("41") + 1; to_decimal("3.5"); to_text(dim)',
     "3\nlist\n42\n3.5\ndim\n"),
    ('forge xs: [text] = ["a", "b"]; xs; xs[1]; length(xs); [] == []',
     '["a", "b"]\nb\n2\nbright\n'),
    ('traverse n in [1, 2, 3, 4] { when n == 2 { skip }; when n == 4 { escape }; reveal(n) }',
     "1\n3\n"),
    ('shift sum = 0; traverse n in [1, 2, 3] { sum += n }; sum', "6\n"),
    ('orbit fact(n: whole) -> whole { when n <= 1 { yield 1 } otherwise { yield n * fact(n - 1) } }; fact(6)',
     "720\n"),
    ('orbit copy(xs: [whole]) -> [whole] { yield xs }; copy([4, 5])', "[4, 5]\n"),
    ('shift n = 0; cycle n < 2 { n += 1; 999; reveal(n) }; when bright { 888; reveal(3) } otherwise { reveal(0) }; { 777 }; n',
     "1\n2\n3\n2\n"),
    ('orbit f() -> whole { 999; yield 4 }; f()', "4\n"),
    ('"summon"; /* summon "missing" */ 4 // summon "missing"', "summon\n4\n"),
    (r'length("a\0b"); ["a\0b"] == ["a\0b"]', "3\nbright\n"),
])
def test_single_line_language_features(aphelion_executable, source, expected):
    result = repl(aphelion_executable, source + "\n")
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected
    assert result.stderr == ""
    assert "aphelion>" not in result.stdout
    assert "REPL" not in result.stdout


def test_declarations_and_functions_do_not_persist(aphelion_executable):
    result = repl(aphelion_executable,
                  "forge x = 10; x\nx\nforge x = 20; x\n"
                  "orbit f() -> whole { yield 3 }; f()\nf()\n42\n")
    assert result.returncode == 1
    assert result.stdout == "10\n20\n3\n42\n"
    assert "undefined name 'x'" in result.stderr
    assert "undefined function 'f'" in result.stderr
    assert "duplicate" not in result.stderr


@pytest.mark.parametrize(("source", "diagnostic"), [
    ("forge bad = ;", "expected an initializer"),
    ('"unterminated', "unterminated"),
    ("unknown_name", "undefined name"),
    ("forge x: whole = 1; x = 2", "immutable"),
    ("1 / 0", "division by zero"),
    ('assert(dim, "repl assertion")', "assertion failed"),
    ('to_whole("bad")', "cannot convert"),
    ("[1][5]", "out of bounds"),
    ("orbit again() -> whole { yield again() }; again()", "limit exceeded"),
    ("1 +", "expected"),
    ("when bright {", "expected"),
    (":unknown", "unknown REPL command"),
])
def test_errors_recover_and_keep_failure_status(aphelion_executable, source, diagnostic):
    result = repl(aphelion_executable, "21\n" + source + "\n6 * 7\n:quit\n")
    assert result.returncode == 1
    assert result.stdout == "21\n42\n"
    assert "<repl>:" in result.stderr
    assert diagnostic in result.stderr


def test_no_multiline_buffer_or_replay(aphelion_executable):
    result = repl(aphelion_executable,
                  'reveal("one time")\nforge x = 1 +\n2\nx\n3\n')
    assert result.returncode == 1
    assert result.stdout == "one time\n2\n3\n"
    assert "expected" in result.stderr
    assert "undefined name" in result.stderr


@pytest.mark.parametrize("source", [
    'summon "missing.agne"',
    '1; summon "missing.agne"',
    '{ summon "missing.agne" }',
    'orbit f() -> silent { summon "missing.agne" }',
])
def test_summon_is_explicitly_file_only(aphelion_executable, source):
    result = repl(aphelion_executable, source + "\n42\n")
    assert result.returncode == 1
    assert result.stdout == "42\n"
    assert "summon is file-only" in result.stderr
    assert "cannot open" not in result.stderr


@pytest.mark.parametrize("source", [b"", b"\n\n", b" \t\r\n// comment\n/* comment */\n; ;\n"])
def test_empty_and_comment_lines_are_harmless(aphelion_executable, source):
    result = repl(aphelion_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == result.stderr == ""


@pytest.mark.parametrize("ending", [b"", b"\n", b"\r\n"])
def test_final_line_and_eof(aphelion_executable, ending):
    result = repl(aphelion_executable, b"40 + 2" + ending)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "42\n"
    assert result.stderr == ""


@pytest.mark.parametrize("command", [":quit", ":q", " \t:q \t\r"])
def test_quit_does_not_evaluate_remaining_input(aphelion_executable, command):
    result = repl(aphelion_executable, "7\n" + command + "\nassert(dim)\n")
    assert result.returncode == 0, result.stderr
    assert result.stdout == "7\n"
    assert result.stderr == ""


def test_help_describes_actual_bounds(aphelion_executable):
    result = repl(aphelion_executable, ":help\n:q\n")
    assert result.returncode == 0, result.stderr
    for text in ("fresh program/scope", "do not persist", "No multiline", "64 KiB",
                 "summon is file-only", "same stdin", ":quit", ":q", "EOF"):
        assert text in result.stdout
    assert result.stderr == ""
    result = subprocess.run([str(aphelion_executable), "--help"], capture_output=True,
                            text=True, timeout=5)
    assert result.returncode == 0
    assert "aphelion repl" in result.stdout


@pytest.mark.parametrize("args", [("extra",), ("--help",), ("--unknown",), ("one", "two")])
def test_repl_rejects_arguments(aphelion_executable, args):
    result = repl(aphelion_executable, b"assert(dim)\n", *args)
    assert result.returncode == 2
    assert result.stdout == ""
    assert "Usage:" in result.stderr


def test_receive_cli_remains_deferred(aphelion_executable):
    result = subprocess.run([str(aphelion_executable), "receive"], capture_output=True,
                            text=True, timeout=5)
    assert result.returncode == 2
    assert "receive is not implemented in this release" in result.stderr


@pytest.mark.parametrize("data", [b"hello\n", b"hello\r\n", b"hello"])
def test_receive_consumes_same_stream(aphelion_executable, data):
    result = repl(aphelion_executable, b"receive()\n" + data)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "hello\n"
    assert result.stderr == ""


def test_receive_then_later_evaluation_and_eof(aphelion_executable):
    result = repl(aphelion_executable, b'receive()\n:q\n6 * 7\nreceive()')
    assert result.returncode == 0, result.stderr
    assert result.stdout == ":q\n42\n\n"
    assert result.stderr == ""


def test_exact_source_byte_limit_is_accepted(aphelion_executable):
    # Source-byte limit, not decoded Unicode character count.
    source = b'"' + "é".encode("utf-8") * ((LIMIT - 2) // 2) + b'"'
    assert len(source) == LIMIT
    result = repl(aphelion_executable, source + b"\n42\n")
    assert result.returncode == 0, result.stderr
    assert result.stdout == source[1:-1].decode("utf-8") + "\n42\n"
    assert result.stderr == ""


@pytest.mark.parametrize("ending", [b"", b"\n42\n"])
def test_overlong_lines_are_drained_without_partial_execution(aphelion_executable, ending):
    result = repl(aphelion_executable, b'reveal("must not execute"); //' + b"x" * (LIMIT * 3) + ending)
    assert result.returncode == 1
    assert result.stdout == ("42\n" if ending else "")
    assert result.stderr.count("exceeds 64 KiB") == 1


@pytest.mark.parametrize("source", [b"\0", b'42\0; reveal("not run")', b"// \0comment",
                                    b"x" * (LIMIT + 1) + b"\0trailing"],
                         ids=["nul-only", "after-expression", "in-comment", "after-limit"])
def test_raw_nul_is_rejected_and_remainder_drained(aphelion_executable, source):
    result = repl(aphelion_executable, source + b"\n7\n")
    assert result.returncode == 1
    assert result.stdout == "7\n"
    assert result.stderr.count("raw NUL") == 1


def test_raw_nul_final_line_is_not_silently_truncated(aphelion_executable):
    result = repl(aphelion_executable, b"42\0")
    assert result.returncode == 1
    assert result.stdout == ""
    assert "raw NUL" in result.stderr


def test_repeated_owned_values_and_failures(aphelion_executable):
    source = ('orbit f(x: text) -> [text] { yield [x + "tail"] }; f("head")[0]\n'
              'forge x = ["discarded"]; assert(dim)\n'
              'forge x = ["fresh"]; length(x)\n') * 150
    result = repl(aphelion_executable, source)
    assert result.returncode == 1
    assert result.stdout == "headtail\n1\n" * 150
    assert result.stderr.count("assertion failed") == 150


def test_input_read_failure_is_diagnosed(aphelion_executable, tmp_path):
    with (tmp_path / "write_only").open("wb") as stream:
        result = subprocess.run([str(aphelion_executable), "repl"], stdin=stream,
                                capture_output=True, timeout=5)
    assert result.returncode == 1, result.stderr
    assert b"input failed" in result.stderr
    assert result.stdout == b""


def test_output_write_failure_is_diagnosed(aphelion_executable, tmp_path):
    path = tmp_path / "read_only"
    path.write_bytes(b"")
    with path.open("rb") as output:
        result = subprocess.run([str(aphelion_executable), "repl"], input=b"42\n",
                                stdout=output, stderr=subprocess.PIPE, timeout=5)
    assert result.returncode == 1, result.stderr
    assert b"output failed" in result.stderr


@pytest.mark.skipif(os.name == "nt", reason="POSIX broken-pipe signal contract")
def test_broken_output_pipe_has_failure_status(aphelion_executable):
    reader, writer = os.pipe()
    os.close(reader)
    try:
        result = subprocess.run([str(aphelion_executable), "repl"], input=b"42\n",
                                stdout=writer, stderr=subprocess.PIPE, timeout=5)
    finally:
        os.close(writer)
    assert result.returncode == 1, result.stderr
    assert b"output failed" in result.stderr


@pytest.mark.skipif(os.name == "nt", reason="/dev/full is a POSIX output-error stream")
def test_output_failure_is_diagnosed(aphelion_executable):
    if not Path("/dev/full").exists():
        pytest.skip("/dev/full unavailable")
    with open("/dev/full", "wb") as output:
        result = subprocess.run([str(aphelion_executable), "repl"], input=b"42\n",
                                stdout=output, stderr=subprocess.PIPE, timeout=5)
    assert result.returncode == 1
    assert b"output failed" in result.stderr


@pytest.mark.skipif(os.name == "nt", reason="native POSIX pseudoterminal test")
def test_native_terminal_banner_prompt_and_flush(aphelion_executable):
    import pty
    import select
    import termios

    master, slave = pty.openpty()
    attributes = termios.tcgetattr(slave)
    attributes[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attributes)
    process = subprocess.Popen([str(aphelion_executable), "repl"], stdin=slave,
                               stdout=slave, stderr=subprocess.PIPE)
    os.close(slave)

    def until_prompt():
        data = b""
        deadline = time.monotonic() + 5
        while b"aphelion> " not in data:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([master], [], [], remaining)[0], data
            data += os.read(master, 4096)
        return data

    try:
        first = until_prompt()  # Must be flushed before any input arrives.
        assert b"Aphelion 0.1.0 REPL" in first
        os.write(master, b"1 + 2\n")
        second = until_prompt()
        assert b"3\r\n" in second
        os.write(master, b":q\n")
        _, errors = process.communicate(timeout=5)
        reject_sanitizer_output(subprocess.CompletedProcess([], process.returncode, first + second, errors))
        assert process.returncode == 0, errors
        assert errors == b""
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
        os.close(master)


@pytest.fixture(scope="session")
def repl_allocation_executable(tmp_path_factory):
    """Real CLI with the existing allocation ledger, plus per-evaluation sweeps."""
    build = tmp_path_factory.mktemp("repl-allocation")
    support = ROOT / "tests" / "support"
    driver = build / "repl_allocation.c"
    driver.write_text(r'''
#include "fail_alloc.h"
#define main original_main
#include "main.c"
#undef main

int main(int argc, char **argv) {
    int status;
    if (argc == 3 && strcmp(argv[1], "sweep") == 0) {
        size_t point, attempts = 0u;
        for (point = 0u; point <= attempts; ++point) {
            bool ok;
            test_alloc_reset(point);
            ok = repl_evaluate(argv[2]);
            if (point == 0u) attempts = test_alloc_attempts();
            if (test_alloc_live() != 0u) {
                fprintf(stderr, "evaluation %zu leaked %zu allocations\n", point, test_alloc_live());
                return 3;
            }
            if ((point == 0u) != ok) {
                fprintf(stderr, "unexpected evaluation result at allocation %zu\n", point);
                return 3;
            }
        }
        printf("swept %zu allocation failures\n", attempts);
        return 0;
    }
    test_alloc_reset(argc == 2 ? (size_t)strtoul(argv[1], NULL, 10) : 0u);
    status = command_repl();
    if (test_alloc_live() != 0u) {
        fprintf(stderr, "REPL leaked %zu allocations\n", test_alloc_live());
        return 3;
    }
    return status;
}
''', encoding="utf-8")
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", f"-I{support}"]
    objects = []
    for source in sorted((ROOT / "src").glob("*.c")):
        if source.name == "main.c":
            continue
        output = build / (source.stem + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(source), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("repl-allocation")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"), str(driver),
                             *math_libraries(), "-o", str(executable)],
                            cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def test_every_evaluation_allocation_failure_is_clean(repl_allocation_executable):
    source = ('orbit make(x: text) -> [text] { yield [x + "tail"] }; '
              'forge xs = make("head"); traverse x in xs { assert(length(x) > 0) }; xs')
    result = subprocess.run([str(repl_allocation_executable), "sweep", source],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert "swept " in result.stdout
    assert "out of memory" in result.stderr


def test_input_allocation_failure_drains_and_recovers(repl_allocation_executable):
    result = subprocess.run([str(repl_allocation_executable), "1"],
                            input=b'reveal("not executed")\n42\n',
                            capture_output=True, timeout=5)
    assert result.returncode == 1, result.stderr
    assert result.stdout.replace(b"\r\n", b"\n") == b"42\n"
    assert b"out of memory reading input" in result.stderr


def test_all_line_state_is_released_after_mixed_repetition(repl_allocation_executable):
    source = (b'forge xs = ["owned"]; xs\n'
              b'forge x = ;\nmissing\n1 / 0\nassert(dim)\n'
              b'/* unclosed\n'
              b'summon "missing"\n'
              b':help\n'
              b'42\0ignored\n'
              + b'//' + b'x' * LIMIT + b'\n') * 20 + b'42'
    result = subprocess.run([str(repl_allocation_executable)], input=source,
                            capture_output=True, timeout=10)
    assert result.returncode == 1, result.stderr
    assert result.stdout.endswith((b"42\n", b"42\r\n"))
    assert b"leaked" not in result.stderr


def test_file_execution_keeps_expression_echo_disabled(aphelion_executable, tmp_path):
    source = tmp_path / "ordinary.agne"
    source.write_text('1 + 2; [1, 2]; reveal("only this")\n', encoding="utf-8")
    result = subprocess.run([str(aphelion_executable), "run", str(source)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "only this\n"
