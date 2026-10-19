"""Immutable scalar lists through the real parser/checker/compiler/VM flow."""

import pytest

from test_aphelion import run_cli, write_program


@pytest.mark.parametrize(("type_name", "elements", "first", "rendered"), [
    ("whole", "1, 2 + 3", "1", "[1, 5]"),
    ("decimal", "1.5, to_decimal(2)", "1.5", "[1.5, 2]"),
    ("signal", "bright, dim", "bright", "[bright, dim]"),
    ("text", '"a", "b" + "c"', "a", '["a", "bc"]'),
    ("glyph", "'a', 'b'", "U+0061", "[U+0061, U+0062]"),
], ids=["w", "d", "s", "t", "g"])
def test_scalar_lists(aphelion_executable, tmp_path, type_name, elements, first, rendered):
    path = write_program(tmp_path, "scalar.agne", f"""
        forge xs: [{type_name}] = [{elements}]
        forge inferred = [{elements}]
        forge empty: [{type_name}] = []
        reveal(xs[0])
        reveal(to_text(inferred))
        reveal(length(xs))
        reveal(length(empty))
        reveal(kind(xs))
        reveal(xs == inferred)
        reveal(empty == [])
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [first, rendered, "2", "0", "list", "bright", "bright"]


def test_empty_contexts(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "empty.agne", """
        orbit empty() -> [text] { yield ([]) }
        orbit size(xs: [text]) -> whole { yield length(xs) }
        shift xs: [text] = ["old"]
        xs = ([])
        reveal(size([]))
        reveal(length(empty()))
        reveal(length(xs))
        reveal(length([]))
        reveal(kind([]))
        reveal([] == [])
        reveal([] == [1])
        reveal([1] != [])
        reveal([])
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["0", "0", "0", "0", "list", "bright", "dim", "bright", "[]"]


def test_order_precedence(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "order.agne", """
        shift calls = 0
        orbit next() -> whole { calls += 1; yield calls }
        orbit make() -> [whole] { yield [next(), next()] }
        orbit index() -> whole { reveal(calls); yield 1 }
        reveal((make())[index()])
        reveal(calls)
        reveal([1, 2][1] + 3 * 2)
        forge xs = [
            1,
            2,
        ]
        reveal(xs[
            1
        ])
        reveal([1, 2] == [2, 1])
        reveal([1, 2] == [1])
        reveal([9007199254740992] != [9007199254740993])
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["2", "2", "2", "8", "2", "dim", "dim", "bright"]


def test_scope_lifetime(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "life.agne", r'''
        forge xs = ["global"]
        orbit make(v: text) -> [text] {
            forge local = [v + "\0tail", "other"]
            yield local
        }
        orbit copy(v: [text]) -> [text] { yield v }
        orbit shadow(xs: [whole]) -> whole {
            when bright { forge xs = ["inner"]; reveal(xs[0]) }
            yield xs[0]
        }
        shift current = copy(make("a"))
        forge old = current
        forge element = current[0]
        current = ["new"]
        { forge xs = [1]; reveal(xs[0]) }
        reveal(xs[0])
        reveal(shadow([9]))
        reveal(old[0] == "a\0tail")
        reveal(element == old[0])
        reveal(length(element))
        shift i = 0
        cycle i < 100 {
            current = make(to_text(i))
            i += 1
        }
        reveal(current[0] == "99\0tail")
        reveal(old[0] == "a\0tail")
        reveal(["a\0b"] != ["a\0c"])
        reveal(length(["a\0b"][0]))
    ''')
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["1", "global", "inner", "9", "bright", "bright",
                                          "6", "bright", "bright", "bright", "3"]


def test_text_rendering(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "render.agne", r'''
        reveal(["a\"b", "c\\d", "x\ny", "\0", "é雪"])
        reveal(to_text(["a\"b", "c\\d", "x\ny", "\0", "é雪"]))
    ''')
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert len(lines) == 2
    assert lines[0] == lines[1]
    assert '\\"' in lines[0] and "\\\\" in lines[0] and "\\n" in lines[0]
    assert "\0" not in result.stdout
    assert "\\0" in lines[0] or "\\x00" in lines[0]
    assert '"é雪"' in lines[0]


def test_anchor_and_wide_literal(aphelion_executable, tmp_path):
    elements = ",".join(str(i) for i in range(1024))
    path = write_program(tmp_path, "wide.agne", f"""
        anchor n = 3
        anchor xs = [n, n + 1]
        forge wide = [{elements}]
        reveal(xs[1])
        reveal(length(wide))
        reveal(wide[1023])
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["4", "1024", "1023"]


@pytest.mark.parametrize(("source", "diagnostic"), [
    ("forge xs = []", "requires a list type annotation"),
    ("forge xs = ([]) ", "requires a list type annotation"),
    ("reveal([][0])", "untyped empty list"),
    ("reveal([1, 2.0])", "exactly one type"),
    ('reveal([1, "x"])', "exactly one type"),
    ("reveal([[1]])", "must be scalar"),
    ("reveal([[]])", "must be scalar"),
    ("reveal([hollow])", "must be scalar"),
    ("reveal([reveal(1)])", "must be scalar"),
    ("orbit f() -> whole { yield 1 }\nreveal([f])", "must be called"),
    ("forge xs: [decimal] = [1]", "expected [decimal]"),
    ("shift xs = [1.0]\nxs = [1]", "cannot assign [whole]"),
    ("orbit f(xs: [decimal]) -> silent { yield }\nf([1])", "expected [decimal]"),
    ("orbit f() -> [decimal] { yield [1] }", "returns [decimal]"),
    ("reveal([1] == [1.0])", "cannot compare"),
    ("reveal([1] == 1)", "cannot compare"),
    ('reveal([1] + [2])', "numeric operands"),
    ("reveal([1] < [2])", "numeric operands"),
    ("reveal([1] and [2])", "signal operands"),
    ("reveal(-[1])", "numeric operand"),
    ('reveal("abc"[0])', "target must be a list"),
    ("reveal(1[0])", "target must be a list"),
    ("reveal([1][0.0])", "index must be whole"),
    ("reveal([1][bright])", "index must be whole"),
    ('reveal([1]["0"])', "index must be whole"),
    ("shift xs = [1]\nxs[0] = 2", "indexed assignment"),
    ("shift xs = [1]\nxs[0] += 2", "indexed assignment"),
    ("forge xs = [1]\nxs = [2]", "immutable name"),
    ("shift xs = [1]\nxs += [2]", "compound assignment"),
    ("forge xs: [whole] = [xs[0]]", "before its initializer"),
    ("reveal([later])\nforge later = 1", "before its initializer"),
    ("{ forge xs = [1] }\nreveal(xs[0])", "undefined name"),
    ("orbit f(xs: [whole]) -> silent { xs = []; yield }", "immutable name"),
    ("forge xs: [silent] = []", "list element type"),
    ("forge xs: [[whole]] = []", "list element type"),
    ("forge xs: [hollow] = []", "list element type"),
    ("forge xs: [] = []", "list element type"),
    ("reveal(to_whole([1]))", "accepts whole, decimal, or text"),
], ids=["empty", "group", "eidx", "mixnum", "mixtext", "nested", "nestempty", "hollow",
        "silent", "func", "widebind", "wideassign", "widearg", "wideret", "eqtype", "eqscalar",
        "add", "order", "logic", "sign", "textidx", "scalaridx", "decidx", "sigidx", "textindex",
        "write", "compound", "forge", "listadd", "self", "early", "scope", "param", "annsilent",
        "annnested", "annhollow", "annempty", "convert"])
@pytest.mark.parametrize("command", ["check", "compile"])
def test_type_errors(aphelion_executable, tmp_path, source, diagnostic, command):
    path = write_program(tmp_path, "bad.agne", source + "\n")
    result = run_cli(aphelion_executable, command, str(path))
    assert result.returncode == 1, result.stdout
    assert diagnostic in result.stderr
    assert f"{path}:" in result.stderr
    assert not path.with_suffix(".apb").exists()


@pytest.mark.parametrize("expression", [
    "[1][-1]", "[1][1]", "[1][9223372036854775807]", "empty[0]",
], ids=["neg", "end", "large", "empty"])
def test_bounds(aphelion_executable, tmp_path, expression):
    path = write_program(tmp_path, "bounds.agne",
                         f"forge empty: [whole] = []\nreveal({expression})\n")
    checked = run_cli(aphelion_executable, "check", str(path))
    assert checked.returncode == 0, checked.stderr
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert result.stdout == ""
    assert "index" in result.stderr.lower()
    assert f"{path}:" in result.stderr


@pytest.mark.parametrize("source", [
    "reveal(" + "[" * 500 + "1" + "]" * 500 + ")",
    "reveal([1]" + "[0]" * 500 + ")",
], ids=["nested", "chain"])
def test_depth_limit(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "deep.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert "limit exceeded" in result.stderr


@pytest.mark.parametrize("source", ["[1,", "[1,,2]", "[1][", "[1][]", "[1 2]"],
                         ids=["open", "comma", "indexopen", "noindex", "sep"])
def test_partial_parse(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "partial.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert f"{path}:" in result.stderr


def test_ast(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "tree.agne", "forge xs = [1, 2]\nreveal(xs[0])\n")
    result = run_cli(aphelion_executable, "ast", str(path))
    assert result.returncode == 0, result.stderr
    assert "list (2 elements)" in result.stdout
    assert "index\n" in result.stdout
    assert "target\n" in result.stdout and "offset\n" in result.stdout
    assert "literal 1" in result.stdout and "name xs" in result.stdout


def test_compile_lists(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "emit.agne", "forge xs = [1, 2]\nreveal(xs[0])\nlength([])\n")
    compiled = run_cli(aphelion_executable, "compile", str(path))
    assert compiled.returncode == 0, compiled.stderr
    bytecode = path.with_suffix(".apb")
    assert bytecode.read_bytes()[:8] == b"APB1\x01\x00\x00\x00"
    disasm = run_cli(aphelion_executable, "disasm", str(bytecode))
    assert disasm.returncode == 0, disasm.stderr
    assert "LIST" in disasm.stdout and "INDEX" in disasm.stdout
