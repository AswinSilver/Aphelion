"""UTF-8 filename handling must agree across CLI, modules, and APB I/O."""

import os
import shutil
import subprocess

import pytest

from conftest import (ROOT, compiler_command, diagnostic_path, math_libraries,
                      native_executable_name, test_cflags as extra_cflags)
from test_aphelion import run_cli


def test_unicode_source_and_bytecode_paths(aphelion_executable, tmp_path):
    directory = tmp_path / "café Ω 🚀"
    directory.mkdir()
    source = directory / "hello 雪.agne"
    source.write_text('reveal("hello")\n', encoding="utf-8")
    checked = run_cli(aphelion_executable, "check", str(source))
    assert checked.returncode == 0, checked.stderr
    assert str(source) in checked.stdout
    executed = run_cli(aphelion_executable, "run", str(source))
    assert executed.returncode == 0, executed.stderr
    assert executed.stdout == "hello\n"
    compiled = run_cli(aphelion_executable, "compile", str(source))
    assert compiled.returncode == 0, compiled.stderr
    bytecode = source.with_suffix(".apb")
    assert bytecode.is_file()
    disassembled = run_cli(aphelion_executable, "disasm", str(bytecode))
    assert disassembled.returncode == 0, disassembled.stderr
    assert str(source) in disassembled.stdout


@pytest.mark.parametrize("command", ["tokens", "ast"])
def test_unicode_inspection_paths(aphelion_executable, tmp_path, command):
    source = tmp_path / "hello café Ω 雪.agne"
    source.write_text('reveal("hello")\n', encoding="utf-8")
    result = run_cli(aphelion_executable, command, str(source))
    assert result.returncode == 0, result.stderr
    assert "hello" in result.stdout
    if command == "ast":
        assert str(source) in result.stdout


def test_unicode_executable_path(aphelion_executable, tmp_path):
    executable = tmp_path / ("aphelion café Ω 雪" + aphelion_executable.suffix)
    shutil.copy2(aphelion_executable, executable)
    result = run_cli(executable, "--version")
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith("aphelion ")


@pytest.mark.parametrize("path", [
    "missing café Ω/hello 雪.agne",
    'missing café Ω/"quoted 雪".agne',
    'missing café Ω/\\"quoted 雪".agne',
    "missing café Ω/hello 🚀.txt",
], ids=["spaces", "quotes", "backslash-quote", "extension"])
def test_argument_spelling_is_preserved_in_diagnostics(aphelion_executable, path):
    result = run_cli(aphelion_executable, "check", path)
    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr.startswith(f"{path}:1:1: ")


def test_unicode_frontend_diagnostic_path(aphelion_executable, tmp_path):
    source = tmp_path / "bad café Ω 雪.agne"
    source.write_text("forge x = ;\n", encoding="utf-8")
    result = run_cli(aphelion_executable, "check", str(source))
    assert result.returncode == 1
    assert result.stderr.startswith(f"{source}:1:")
    assert "expected an initializer" in result.stderr


@pytest.mark.skipif(os.name != "nt", reason="native Windows UTF-16 command line")
@pytest.mark.parametrize("surrogate", ["\ud800", "\udc00"], ids=["high-surrogate", "low-surrogate"])
def test_invalid_utf16_argument_is_rejected(aphelion_executable, surrogate):
    result = run_cli(aphelion_executable, "check", f"bad{surrogate}.agne")
    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr == "aphelion: process arguments contain invalid UTF-16\n"


@pytest.fixture(scope="session")
def windows_argument_allocation_executable(tmp_path_factory):
    if os.name != "nt":
        pytest.skip("native Windows command-line allocation handling")
    build = tmp_path_factory.mktemp("windows-argument-allocation")
    support = ROOT / "tests" / "support"
    driver = build / "argument_allocation.c"
    driver.write_text(r'''
#include "fail_alloc.h"
#define main original_main
#include "main.c"
#undef main

int main(void) {
    size_t point, attempts = 0u;
    for (point = 0u; point <= attempts; ++point) {
        int argc;
        char **argv;
        bool ok;
        test_alloc_reset(point);
        ok = windows_utf8_arguments(&argc, &argv);
        if (point == 0u) attempts = test_alloc_attempts();
        if (ok && (argc != 3 || argv[argc] != NULL)) {
            fputs("invalid normalized argument vector\n", stderr);
            windows_free_arguments(argc, argv);
            return 3;
        }
        if (!ok && (argc != 0 || argv != NULL)) {
            fputs("failed conversion published partial arguments\n", stderr);
            return 3;
        }
        windows_free_arguments(argc, argv);
        if (test_alloc_live() != 0u) {
            fprintf(stderr, "argument conversion %zu leaked %zu allocations\n",
                    point, test_alloc_live());
            return 3;
        }
        if ((point == 0u) != ok) {
            fprintf(stderr, "unexpected argument conversion result at allocation %zu\n", point);
            return 3;
        }
    }
    printf("swept %zu allocation failures\n", attempts);
    return 0;
}
''', encoding="utf-8")
    executable = build / native_executable_name("argument-allocation")
    command = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
               *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", f"-I{support}",
               *(str(source) for source in sorted((ROOT / "src").glob("*.c"))
                 if source.name != "main.c"),
               str(support / "fail_alloc.c"), str(driver), *math_libraries(),
               "-o", str(executable)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def test_every_windows_argument_allocation_failure_is_clean(windows_argument_allocation_executable):
    result = run_cli(windows_argument_allocation_executable, "café Ω 雪", 'quoted \\"Ω"')
    assert result.returncode == 0, result.stderr
    assert "swept " in result.stdout
    assert "out of memory reading process arguments" in result.stderr


def test_module_under_unicode_root_preserves_diagnostic_path(aphelion_executable, tmp_path):
    directory = tmp_path / "modules Ω 雪"
    directory.mkdir()
    entry = directory / "entry.agne"
    dependency = directory / "dep.agne"
    entry.write_text('summon "dep"\nreveal(f())\n', encoding="utf-8")
    dependency.write_text('orbit f() -> whole { yield 1 / 0 }\n', encoding="utf-8")
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 1
    assert diagnostic_path(dependency) in result.stderr
    assert "division by zero" in result.stderr
