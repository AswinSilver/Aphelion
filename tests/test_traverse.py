"""Traversal contract through real .agne parsing, checking, lowering, and VM use."""

import pytest

from test_aphelion import run_cli, write_program


@pytest.mark.parametrize(("type_name", "values", "expected"), [
    ("whole", "1, 2 + 3, -4", ["1", "5", "-4"]),
    ("decimal", "1.5, to_decimal(2), -0.5", ["1.5", "2", "-0.5"]),
    ("signal", "bright, dim, not dim", ["bright", "dim", "bright"]),
    ("text", '"a", "b" + "c", ""', ["a", "bc", ""]),
    ("glyph", "'a', 'b', 'c'", ["U+0061", "U+0062", "U+0063"]),
], ids=["w", "d", "s", "t", "g"])
@pytest.mark.parametrize("size", [0, 1, 3], ids=["empty", "one", "many"])
def test_typed_lists(aphelion_executable, tmp_path, type_name, values, expected, size):
    elements = ",".join(values.split(",")[:size])
    path = write_program(tmp_path, "typed.agne", f"""
        forge xs: [{type_name}] = [{elements}]
        shift count = 0
        traverse item in xs {{
            reveal(item)
            assert(kind(item) == "{type_name}")
            count += 1
        }}
        reveal(count)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == expected[:size] + [str(size)]


def test_typed_results_and_expression_iterables(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "expr.agne", """
        orbit empty() -> [whole] { yield [] }
        orbit copy(xs: [whole]) -> [whole] { yield xs }
        traverse n in empty() { assert(dim) }
        traverse n in copy([]) { assert(dim) }
        traverse n in ([2, 4, 6]) { reveal(n / 2) }
        shift xs: [whole] = []
        traverse n in (xs = [7, 8]) { reveal(n) }
        reveal(length(xs))
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "2", "3", "7", "8", "2"]


def test_snapshot_and_evaluate_once(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "snapshot.agne", """
        shift calls = 0
        shift xs = [1, 2, 3]
        orbit make() -> [whole] { calls += 1; yield xs }
        traverse n in make() {
            reveal(n)
            xs = [99]
        }
        reveal(calls)
        reveal(xs[0])
        xs = [4, 5]
        traverse n in xs { xs = []; reveal(n) }
        reveal(length(xs))
        traverse n in make() { assert(dim) }
        reveal(calls)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "2", "3", "1", "99", "4", "5", "0", "2"]


def test_binding_scopes_and_outer_iterable(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "scope.agne", """
        forge n = [1, 2]
        traverse n in n {
            reveal(n)
            { forge n = "inner"; reveal(n) }
            reveal(n)
        }
        reveal(n[1])
        traverse n in [3] { forge n = "body"; reveal(n) }
        orbit local(n: [whole]) -> whole {
            traverse n in n { reveal(n) }
            yield n[0]
        }
        reveal(local([7, 8]))
        traverse n in [9] {
            traverse n in [n, n + 1] { reveal(n) }
            reveal(n)
        }
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "inner", "1", "2", "inner", "2", "2",
                                          "body", "7", "8", "7", "9", "10", "9"]


def test_escape_and_skip_advance(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "control.agne", """
        shift seen = 0
        traverse n in [1, 2, 3, 4, 5] {
            seen += 1
            when n == 2 { skip }
            when n == 4 { escape }
            reveal(n)
        }
        reveal(seen)
        traverse n in [8, 9] { skip }
        traverse n in [8, 9] { escape }
        reveal("done")
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "3", "4", "done"]


def test_nested_traverse_nearest_loop(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "nested.agne", """
        traverse a in [1, 2] {
            traverse b in [1, 2, 3, 4] {
                when b == 2 { skip }
                when b == 4 { escape }
                reveal(a * 10 + b)
            }
            reveal(a)
        }
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["11", "13", "1", "21", "23", "2"]


def test_mixed_loops_nearest_control(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "mixed.agne", """
        traverse a in [1, 2] {
            shift b = 0
            cycle b < 4 {
                b += 1
                when b == 2 { skip }
                when b == 4 { escape }
                reveal(a * 10 + b)
            }
            when a == 1 { skip }
            reveal(99)
        }
        shift c = 0
        cycle c < 3 {
            c += 1
            traverse d in [1, 2, 3, 4] {
                when d == 2 { skip }
                when d == 4 { escape }
                reveal(c * 100 + d)
            }
            when c == 2 { escape }
        }
        reveal(c)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["11", "13", "21", "23", "99", "101", "103",
                                          "201", "203", "2"]


def test_text_snapshot_and_retained_element(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "text.agne", r'''
        orbit make(v: text) -> [text] { yield [v + "\0tail", "second"] }
        shift xs = make("old")
        shift saved = ""
        traverse word in xs {
            xs = make("new")
            when word == "second" { skip }
            saved = word
        }
        assert(saved == "old\0tail")
        assert(xs[0] == "new\0tail")
        traverse word in make("temporary") { saved = word; escape }
        assert(saved == "temporary\0tail")
        orbit first() -> text {
            traverse word in make("return") { yield word }
            yield "empty"
        }
        saved = first()
        shift i = 0
        cycle i < 1000 {
            traverse word in make(to_text(i)) {
                forge local = word + "!"
                when word == "second" { escape }
                skip
            }
            i += 1
        }
        assert(saved == "return\0tail")
        reveal(length(saved))
        reveal("retained")
    ''')
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["11", "retained"]


def test_yield_and_fallback(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "yield.agne", """
        orbit first(xs: [whole]) -> whole {
            traverse n in xs { yield n }
            yield -1
        }
        orbit widen() -> decimal {
            traverse n in [5] { yield n }
            yield 0.0
        }
        orbit stop() -> silent {
            traverse n in [1, 2] { reveal(n); yield }
            reveal(99)
        }
        orbit nested() -> whole {
            traverse a in [1, 2] {
                cycle bright { traverse b in [3, 4] { yield a + b } }
            }
            yield 0
        }
        reveal(first([]))
        reveal(first([8, 9]))
        reveal(widen() / 2)
        stop()
        reveal(nested())
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["-1", "8", "2.5", "1", "4"]


@pytest.mark.parametrize(("source", "diagnostic"), [
    ('traverse n in "abc" {}', "must be a typed list"),
    ("traverse n in 1 {}", "must be a typed list"),
    ("traverse n in 1.0 {}", "must be a typed list"),
    ("traverse n in bright {}", "must be a typed list"),
    ("traverse n in 'a' {}", "must be a typed list"),
    ("traverse n in hollow {}", "must be a typed list"),
    ("traverse n in reveal(1) {}", "must be a typed list"),
    ("traverse n in [] {}", "untyped empty list"),
    ("traverse n in ([]) {}", "untyped empty list"),
    ("traverse n in [[1]] {}", "must be scalar"),
    ("traverse n in [1, 2.0] {}", "exactly one type"),
    ("traverse n in [1] { n = 2 }", "immutable name 'n'"),
    ("traverse n in [1] { n += 1 }", "immutable name 'n'"),
    ("traverse n in [1] { anchor a = n }", "compile-time constant"),
    ("traverse n in [1] { anchor a = [n] }", "compile-time constant"),
    ("traverse n in [1] {}\nreveal(n)", "undefined name 'n'"),
    ("traverse n in [n] {}", "undefined name 'n'"),
    ("traverse n in xs {}\nforge xs = [1]", "before its initializer"),
    ("traverse length in [1] {}", "cannot be redeclared"),
    ("traverse n in [1] {}\nescape", "only valid inside"),
    ("traverse n in [1] {}\nskip", "only valid inside"),
    ("traverse n in [1] { yield n }", "only valid inside an orbit"),
    ("orbit f() -> whole { traverse n in [1] { yield n } }", "every path"),
    ("orbit f(xs: [whole]) -> whole { traverse n in xs { yield n } }", "every path"),
    ('orbit f() -> whole { traverse n in ["a"] { yield n }; yield 0 }', "yield has type text"),
    ("orbit f() -> silent { traverse n in [1] { yield n } }", "cannot yield a value"),
    ("orbit f() -> whole { traverse n in [1] { yield }; yield 0 }", "must yield a whole"),
    ("orbit f() -> silent { escape }\ntraverse n in [1] { f() }", "only valid inside"),
], ids=["text", "whole", "dec", "sig", "glyph", "null", "silent", "empty", "group",
        "nested", "mixed", "assign", "add", "anchor", "alist", "scope", "self", "early",
        "builtin", "escape", "skip", "topret", "noyield", "emptyret", "badret", "sret",
        "bare", "fnloop"])
@pytest.mark.parametrize("command", ["check", "compile"])
def test_semantic_errors(aphelion_executable, tmp_path, source, diagnostic, command):
    path = write_program(tmp_path, "bad.agne", source + "\n")
    result = run_cli(aphelion_executable, command, str(path))
    assert result.returncode == 1, result.stdout
    assert diagnostic in result.stderr
    assert f"{path}:" in result.stderr
    assert not path.with_suffix(".apb").exists()


@pytest.mark.parametrize("source", [
    "traverse", "traverse in [1] {}", "traverse 1 in [1] {}",
    "traverse forge in [1] {}", "traverse n [1] {}", "traverse n in",
    "traverse n in {}", "traverse n in [1]", "traverse n in [1] reveal(n)",
    "traverse n in [1] {", "traverse n in [1] { reveal(n)",
    "traverse n: whole in [1] {}", "traverse a, b in [1] {}",
    "traverse n in [1, {}", "traverse n in [1][ {}",
    "{ traverse n in [1] { reveal(n) } otherwise {} }",
    "traverse n in [1] {} reveal(2)",
], ids=["kw", "name", "num", "reserved", "in", "expr", "noexpr", "block", "brace",
        "close", "partial", "ann", "pair", "list", "index", "otherwise", "topsep"])
@pytest.mark.parametrize("command", ["ast", "check"])
def test_malformed_syntax(aphelion_executable, tmp_path, source, command):
    path = write_program(tmp_path, "syntax.agne", source)
    result = run_cli(aphelion_executable, command, str(path))
    assert result.returncode == 1, result.stdout
    assert f"{path}:" in result.stderr


def test_block_boundaries_and_ast(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "ast.agne", """
        {
            traverse n in [1] { reveal(n) } traverse n in [2] { reveal(n) }
            reveal(3)
        }
    """)
    result = run_cli(aphelion_executable, "ast", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("traverse\n") == 2
    assert result.stdout.count("binding forge n") == 2
    assert result.stdout.count("iterable\n") == 2
    assert "name n" in result.stdout and "list (1 elements)" in result.stdout
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "2", "3"]


def test_normal_nesting(aphelion_executable, tmp_path):
    source = "traverse n in [1] {" * 24 + "reveal(n)" + "}" * 24 + "\n"
    path = write_program(tmp_path, "nest.agne", source)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "1\n"


@pytest.mark.parametrize("source", [
    "traverse n in [1] {" * 1000 + "reveal(n)" + "}" * 1000,
    "traverse n in " + "(" * 1000 + "[1]" + ")" * 1000 + " {}",
], ids=["body", "iter"])
def test_depth_limit(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "deep.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1, result.stderr
    assert "limit exceeded" in result.stderr


@pytest.mark.parametrize("body", ["assert(dim)", "cycle bright {}"], ids=["error", "budget"])
def test_runtime_failure_in_body(aphelion_executable, tmp_path, body):
    path = write_program(tmp_path, "runtime.agne", f'traverse word in ["owned"] {{ {body} }}\n')
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert f"{path}:" in result.stderr
    diagnostic = "assertion failed" if body.startswith("assert") else "limit"
    assert diagnostic in result.stderr


def test_compile_and_disassemble(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "emit.agne", "traverse n in [1, 2] { reveal(n); skip }\n")
    result = run_cli(aphelion_executable, "compile", str(path))
    assert result.returncode == 0, result.stderr
    bytecode = path.with_suffix(".apb")
    assert bytecode.read_bytes()[:8] == b"APB1\x01\x00\x00\x00"
    result = run_cli(aphelion_executable, "disasm", str(bytecode))
    assert result.returncode == 0, result.stderr
    for operation in ("LIST", "INDEX", "LENGTH", "JUMP"):
        assert operation in result.stdout
