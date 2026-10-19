"""REPL expression echo through the public compiler API and ordinary bytecode VM."""

import re
import subprocess

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)


@pytest.fixture(scope="session")
def repl_vm_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("repl-vm")
    support = ROOT / "tests" / "support"
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("ast", "token", "lexer", "parser", "semantic", "module",
                 "compiler", "file", "value", "bytecode", "vm"):
        output = build / (name + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(ROOT / "src" / (name + ".c")), "-o", str(output)],
                                cwd=ROOT, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("repl-checks")
    result = subprocess.run([*flags, *objects, str(support / "fail_alloc.c"),
                             str(support / "repl_checks.c"), *math_libraries(),
                             "-o", str(executable)], cwd=ROOT, capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def run_api(executable, source, mode="--echo", save=None):
    args = [str(executable), mode, str(source)]
    if save is not None:
        args.append(str(save))
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    assert result.returncode in (0, 1), result.stderr
    return result


def source_file(tmp_path, source):
    path = tmp_path / "original.agne"
    path.write_text(source, encoding="utf-8")
    return path


def instructions(disassembly):
    return [(int(line), int(column), opcode, int(operand))
            for line, column, opcode, operand in re.findall(
                r"^\s+\d+\s+(\d+):(\d+)\s+(\w+)\s+(-?\d+)", disassembly, re.MULTILINE)]


def test_default_options_and_null_preserve_file_output_and_identical_bytecode(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, '1 + 2; [3, 4]; "ignored"; reveal("explicit"); assert(bright)\n')
    default_path, null_path = tmp_path / "default.apb", tmp_path / "null.apb"
    for mode, bytecode in [("--default", default_path), ("--null", null_path)]:
        result = run_api(repl_vm_executable, source, mode, bytecode)
        assert result.returncode == 0, result.stderr
        assert result.stdout == "explicit\n"
        assert result.stderr == ""
    assert default_path.read_bytes() == null_path.read_bytes()
    plain = run_api(repl_vm_executable, source, "--default-inspect")
    assert plain.returncode == 0, plain.stderr
    assert [op for _, _, op, _ in instructions(plain.stdout)].count("REVEAL") == 1


@pytest.mark.parametrize(("expression", "expected"), [
    ("1 + 2 * 3", "7\n"), ("1.25", "1.25\n"), ("bright", "bright\n"),
    ("not bright", "dim\n"), ("'A'", "U+0041\n"), ("hollow", "hollow\n"),
    ('"a\\0tail"', "a\0tail\n"), ('""', "\n"),
    ("[1, 2 + 3]", "[1, 5]\n"), ("[]", "[]\n"),
    ('["a\\0tail", "b"]', '["a\\0tail", "b"]\n'),
    ('["a\\0tail"][0]', "a\0tail\n"),
])
def test_opt_in_echo_uses_existing_value_rendering(repl_vm_executable, tmp_path, expression, expected):
    source = source_file(tmp_path, expression + "\n")
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected
    assert result.stderr == ""


def test_assignments_echo_once_and_declarations_do_not_echo(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, r'''
        shift n = 1
        forge unused = 999
        n = 4
        n += 3
        n
        shift xs = ["old\0tail"]
        xs
        xs = ["new"]
        xs
        assert(n == 7)
        assert(xs[0] == "new")
    ''')
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == '4\n7\n7\n["old\\0tail"]\n["new"]\n["new"]\n'


def test_reveal_assert_groups_and_silent_functions_are_not_echoed_again(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, '''
        orbit quiet() -> silent { 999; reveal("body"); yield }
        reveal("explicit")
        (reveal("grouped"))
        assert(bright)
        (assert(bright, "unused"))
        quiet()
        (quiet())
        "silent"
        "hollow"
    ''')
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "explicit\ngrouped\nbody\nbody\nsilent\nhollow\n"


def test_only_direct_entry_statements_echo_nested_bodies_keep_behavior(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, '''
        orbit value() -> whole { 100; { 101 }; yield 7 }
        forge initializer = value()
        { 200; forge local = 201; local }
        when bright { 300 } otherwise { 301 }
        when dim { 302 } otherwise { 303 }
        shift i = 0
        cycle i < 2 { i += 1; 400; when bright { 401 } }
        traverse x in [500, 501] { x; { x + 1 } }
        value()
        i
        600
    ''')
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "7\n2\n600\n"


def test_many_owned_expressions_are_balanced_under_a_small_vm_stack_limit(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, ('"text\\0tail"; ["list", "item"]; 42; assert(bright);\n' * 500))
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 0, result.stderr
    assert result.stdout == ('text\0tail\n["list", "item"]\n42\n' * 500)


def test_echo_bytecode_contains_reveal_then_pop_with_original_locations(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, '  123\n    "text"\n')
    default = run_api(repl_vm_executable, source, "--default-inspect")
    assert default.returncode == 0, default.stderr
    assert instructions(default.stdout) == [
        (1, 3, "CONSTANT", 0), (1, 3, "POP", 0),
        (2, 5, "CONSTANT", 1), (2, 5, "POP", 0),
        (1, 1, "HOLLOW", 0), (1, 1, "RETURN", 0), (1, 1, "HALT", 0),
    ]
    result = run_api(repl_vm_executable, source, "--inspect")
    assert result.returncode == 0, result.stderr
    assert instructions(result.stdout) == [
        (1, 3, "CONSTANT", 0), (1, 3, "REVEAL", 0), (1, 3, "POP", 0),
        (2, 5, "CONSTANT", 1), (2, 5, "REVEAL", 0), (2, 5, "POP", 0),
        (1, 1, "HOLLOW", 0), (1, 1, "RETURN", 0), (1, 1, "HALT", 0),
    ]


def test_no_source_or_output_rewriting_and_original_runtime_location(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, '// reveal(99) is a comment\n'
                         '"reveal(77); // literal"\n"before\\nsilent\\nafter"\n  1 / 0\n')
    original = source.read_bytes()
    result = run_api(repl_vm_executable, source)
    assert result.returncode == 1
    assert result.stdout == "reveal(77); // literal\nbefore\nsilent\nafter\n"
    assert f"{source}:4:3:" in result.stderr
    assert "zero" in result.stderr
    assert source.read_bytes() == original


def test_linked_source_markers_and_ast_lifetime_are_preserved(repl_vm_executable, tmp_path):
    dependency = tmp_path / "dep.agne"
    dependency.write_text('orbit f() -> whole { 99; yield 7 }\n', encoding="utf-8")
    source = source_file(tmp_path, 'summon "dep"\nf()\n"a\\0tail"\n')
    result = run_api(repl_vm_executable, source, "--linked")
    assert result.returncode == 0, result.stderr
    assert result.stdout == "7\na\0tail\n"
    inspected = run_api(repl_vm_executable, source, "--linked-inspect")
    assert inspected.returncode == 0, inspected.stderr
    ops = [op for _, _, op, _ in instructions(inspected.stdout)]
    assert ops.count("SOURCE") == 6
    assert ops.count("REVEAL") == 2
    source.write_text('summon "dep"\n"ok"\n  1 / 0\n', encoding="utf-8")
    failed = run_api(repl_vm_executable, source, "--linked")
    assert failed.returncode == 1
    assert f"{source.resolve()}:3:3:" in failed.stderr


def test_echo_bytecode_roundtrip_is_version_one_and_reexecutes_identically(repl_vm_executable, tmp_path):
    source = source_file(tmp_path, 'shift xs = ["a\\0tail", "b"]; xs; xs[0]; assert(bright)\n')
    original, rewritten = tmp_path / "echo.apb", tmp_path / "roundtrip.apb"
    result = run_api(repl_vm_executable, source, save=original)
    assert result.returncode == 0, result.stderr
    assert original.read_bytes()[4:8] == b"\x01\x00\x00\x00"
    loaded = run_api(repl_vm_executable, original, "--bytecode", rewritten)
    assert loaded.returncode == 0, loaded.stderr
    assert loaded.stdout == result.stdout == '["a\\0tail", "b"]\na\0tail\n'
    assert original.read_bytes() == rewritten.read_bytes()


@pytest.mark.parametrize("tail", ["", 'assert(dim, "owned failure\\0tail")\n',
                                  '["owned"][2]\n'])
def test_ownership_and_oom_cleanup_through_frontend_compiler_and_vm(repl_vm_executable, tmp_path, tail):
    source = source_file(tmp_path, '''
        orbit copy(v: text) -> [text] { "discarded"; yield [v + "\\0tail", v] }
        shift xs = copy("old")
        xs
        xs = copy("new")
        xs[0]
        (assert(bright))
        traverse word in xs { word; xs = ["replacement"]; reveal(word) }
    ''' + ('["echo", "owned\\0tail"];\n' * 12) + tail)
    result = run_api(repl_vm_executable, source, "--sweep")
    assert result.returncode == (1 if tail else 0), result.stderr
    assert "REPL allocation cleanup checks passed" in result.stdout
