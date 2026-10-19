"""Builtin contracts through the parser, semantic checker, compiler and VM."""

import re

import pytest

from test_aphelion import run_cli, write_program


@pytest.mark.parametrize(("expression", "expected"), [
    ('""', 0),
    ('"abc"', 3),
    ('"é雪🙂"', 9),
    ('"é"', 3),
    ('"\\0"', 1),
    ('"a\\0b\\x00"', 4),
    ('"\\0" + "é\\0"', 4),
], ids=["empty", "ascii", "utf8", "comb", "nul", "nuls", "concat"])
def test_len(aphelion_executable, tmp_path, expression, expected):
    path = write_program(tmp_path, "len.agne", f"""
        forge n: whole = (length)({expression})
        reveal(n)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{expected}\n"


def test_len_life(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "life.agne", """
        orbit make(v: text) -> text { yield v + "\\0雪" }
        shift value = make("é")
        forge old = value
        value = value + "x"
        reveal(length(old))
        reveal(length(value))
        reveal(length(receive()))
    """)
    result = run_cli(aphelion_executable, "run", str(path), input_text="aé\n")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["6", "7", "3"]


def test_kinds(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "kind.agne", """
        orbit quiet() -> silent { yield }
        orbit wide() -> decimal { yield 1 }
        forge wide_value: decimal = 1
        forge hole = hollow
        forge label: text = kind(1)
        reveal(label)
        reveal((kind)(1.5))
        reveal(kind(bright))
        reveal(kind("a\\0b"))
        reveal(kind('x'))
        reveal(kind(hole))
        reveal(kind(quiet()))
        reveal(kind(assert(bright)))
        reveal(kind(reveal("side")))
        reveal(kind(wide_value))
        reveal(kind(wide()))
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "whole", "decimal", "signal", "text", "glyph", "hollow", "silent",
        "silent", "side", "silent", "decimal", "decimal",
    ]


def test_eval_once(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "once.agne", """
        shift calls = 0
        orbit text_value() -> text { calls += 1; yield "abc" }
        orbit whole_value() -> whole { calls += 1; yield 42 }
        reveal(length(text_value()))
        reveal(kind(whole_value()))
        reveal(calls)
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["3", "whole", "2"]


@pytest.mark.parametrize("condition", ["bright", "dim"], ids=["ok", "fail"])
def test_assert_order(aphelion_executable, tmp_path, condition):
    path = write_program(tmp_path, "order.agne", f"""
        shift calls = 0
        orbit condition() -> signal {{
            calls += 1
            reveal("condition")
            yield {condition}
        }}
        orbit message() -> text {{
            calls += 1
            reveal(calls)
            yield "ordered failure"
        }}
        (assert)(condition(), message())
        reveal("after")
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    if condition == "bright":
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ["condition", "2", "after"]
    else:
        assert result.returncode == 1
        assert result.stdout.splitlines() == ["condition", "2"]
        assert "ordered failure" in result.stderr
        assert "assert" in result.stderr.lower()
        assert f"{path}:" in result.stderr


def test_assert_ok(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "ok.agne", """
        assert(bright)
        assert(2 < 3, "")
        reveal(kind(assert(bright, "unused")))
        reveal("after")
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["silent", "after"]
    assert result.stderr == ""


@pytest.mark.parametrize(("expression", "message"), [
    ("assert(dim)", None),
    ('assert(dim, "custom failure")', "custom failure"),
    ('assert(dim, "")', None),
    ('assert(dim, "a\\0b")', "a\\0b"),
], ids=["plain", "msg", "empty", "nul"])
def test_assert_fail(aphelion_executable, tmp_path, expression, message):
    path = write_program(tmp_path, "fail.agne", f'{expression}\nreveal("after")\n')
    checked = run_cli(aphelion_executable, "check", str(path))
    assert checked.returncode == 0, checked.stderr
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert result.stdout == ""
    assert "assert" in result.stderr.lower()
    assert f"{path}:1:1:" in result.stderr
    if message is not None:
        assert message in result.stderr


@pytest.mark.parametrize("expression", [
    "reveal()",
    'reveal(1, "extra")',
], ids=["missing", "extra"])
def test_reveal_requires_one_argument(aphelion_executable, tmp_path, expression):
    path = write_program(tmp_path, "bad_reveal.agne", expression + "\n")

    result = run_cli(aphelion_executable, "check", str(path))

    assert result.returncode == 1
    assert "reveal expects exactly one argument" in result.stderr


@pytest.mark.parametrize("condition", ["bright", "dim"], ids=["ok", "fail"])
def test_msg_error(aphelion_executable, tmp_path, condition):
    path = write_program(tmp_path, "eager.agne",
                         f'assert({condition}, to_text(1 / 0))\n')
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert "division by zero" in result.stderr
    assert "assertion failed" not in result.stderr.lower()
    assert f"{path}:" in result.stderr


@pytest.mark.parametrize(("expression", "diagnostic"), [
    ("length()", "exactly one argument"),
    ('length("a", "b")', "exactly one argument"),
    ("kind()", "exactly one argument"),
    ("kind(1, 2)", "exactly one argument"),
    ("assert()", "one or two arguments"),
    ('assert(bright, "a", "b")', "one or two arguments"),
    ("length(1)", "expected text"),
    ("length(1.5)", "expected text"),
    ("length(bright)", "expected text"),
    ("length('x')", "expected text"),
    ("length(hollow)", "expected text"),
    ("length(reveal(1))", "expected text"),
    ("assert(1)", "expected signal"),
    ('assert("bright")', "expected signal"),
    ("assert(hollow)", "expected signal"),
    ("assert(reveal(1))", "expected signal"),
    ("assert(bright, 1)", "expected text"),
    ("assert(bright, hollow)", "expected text"),
    ("assert(bright, reveal(1))", "expected text"),
], ids=["l0", "l2", "k0", "k2", "a0", "a3", "lw", "ld", "ls", "lg",
        "lh", "lv", "aw", "at", "ah", "av", "mw", "mh", "mv"])
@pytest.mark.parametrize("command", ["check", "compile"])
def test_bad_call(aphelion_executable, tmp_path, expression, diagnostic, command):
    path = write_program(tmp_path, "bad.agne", expression + "\n")
    result = run_cli(aphelion_executable, command, str(path))
    assert result.returncode == 1
    assert diagnostic in result.stderr
    assert f"{path}:" in result.stderr
    assert not path.with_suffix(".apb").exists()


@pytest.mark.parametrize("builtin", ["length", "kind", "assert"])
@pytest.mark.parametrize("template", [
    "forge {name} = 1\n",
    "shift {name} = 1\n",
    "anchor {name} = 1\n",
    "{{ forge {name} = 1 }}\n",
    "when bright {{ shift {name} = 1 }}\n",
    "cycle dim {{ anchor {name} = 1 }}\n",
    "orbit f() -> silent {{ forge {name} = 1 }}\n",
    "orbit f() -> silent {{ when bright {{ forge {name} = 1 }} }}\n",
    "orbit f({name}: text) -> silent {{ yield }}\n",
    "orbit {name}() -> silent {{ yield }}\n",
], ids=["forge", "shift", "anchor", "block", "when", "cycle", "local",
        "nested", "param", "orbit"])
def test_reserved(aphelion_executable, tmp_path, builtin, template):
    path = write_program(tmp_path, "shadow.agne", template.format(name=builtin))
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert f"builtin name '{builtin}' cannot be redeclared" in result.stderr


@pytest.mark.parametrize("source", [
    "reveal(kind(later))\nforge later = 1\n",
    "forge value = kind(value)\n",
    "orbit f() -> whole { yield 1 }\nreveal(kind(f))\n",
    "forge value = assert(bright)\n",
    "forge n: text = length(\"x\")\n",
    "forge n: whole = kind(1)\n",
    "kind()\nreveal(missing)\n",
], ids=["early", "self", "func", "silent", "ltype", "ktype", "missing"])
def test_bad_value(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "value.agne", source)
    result = run_cli(aphelion_executable, "check", str(path))
    assert result.returncode == 1
    assert f"{path}:" in result.stderr


def test_uninit(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "early.agne", """
        orbit inspect() -> text { yield kind(later) }
        reveal(inspect())
        forge later = 1
    """)
    result = run_cli(aphelion_executable, "run", str(path))
    assert result.returncode == 1
    assert result.stdout == ""
    assert "before initialization" in result.stderr


def test_emit(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "emit.agne", """
        reveal(length("a\\0b"))
        reveal(kind(hollow))
        assert(bright)
        assert(bright, "message")
    """)
    compiled = run_cli(aphelion_executable, "compile", str(path))
    assert compiled.returncode == 0, compiled.stderr
    bytecode = path.with_suffix(".apb")
    assert bytecode.read_bytes()[:8] == b"APB1\x01\x00\x00\x00"
    disasm = run_cli(aphelion_executable, "disasm", str(bytecode))
    assert disasm.returncode == 0, disasm.stderr
    assert re.findall(r"\b(LENGTH|KIND|ASSERT)\s+(\d+)\s*$",
                      disasm.stdout, re.MULTILINE) == [
        ("LENGTH", "0"), ("KIND", "0"), ("ASSERT", "1"), ("ASSERT", "2"),
    ]
