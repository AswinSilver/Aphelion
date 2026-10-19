"""Independent source modules: graph order, boundaries, ownership, and limits."""

import os
import subprocess

import pytest

from conftest import (ROOT, compiler_command, diagnostic_path, native_executable_name,
                      test_cflags as extra_cflags)
from test_aphelion import run_cli
from test_module_vm import source_executable  # standalone .apb runner


def module(root, name, source):
    path = root / (name + ".agne")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source, encoding="utf-8")
    return path


def test_nested_root_relative_imports_and_dependency_order(aphelion_executable, tmp_path):
    module(tmp_path, "math/util", 'reveal("util")\nforge base = 6\n')
    module(tmp_path, "nested/branch", 'summon "math/util"\nreveal("branch")\n'
           'orbit answer() -> whole { yield base * 7 }\n')
    entry = module(tmp_path, "entry", 'summon "nested/branch"\nreveal(answer())\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "util\nbranch\n42\n"
    result = run_cli(aphelion_executable, "ast", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout.index("literal util") < result.stdout.index("literal branch")
    assert "summon" not in result.stdout


def test_diamond_and_repeated_imports_initialize_once(aphelion_executable, tmp_path):
    module(tmp_path, "shared", 'reveal("shared")\nforge amount = 8\n')
    module(tmp_path, "left", 'summon "shared"\nreveal("left")\n')
    module(tmp_path, "right", 'summon "shared"\nreveal("right")\n')
    entry = module(tmp_path, "entry", 'summon "left"\nsummon "right"\n'
                   'summon "left"\nsummon "shared"\nreveal(amount)\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "shared\nleft\nright\n8\n"


def test_all_functions_are_linked_before_initializers(aphelion_executable, tmp_path):
    module(tmp_path, "dep", "forge result = root_function()\n")
    entry = module(tmp_path, "entry", 'summon "dep"\n'
        'orbit root_function() -> whole { yield 42 }\nreveal(result)\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "42\n"


def test_early_cross_module_global_read_still_fails(aphelion_executable, tmp_path):
    module(tmp_path, "dep", "forge result = root_function()\n")
    entry = module(tmp_path, "entry", 'summon "dep"\n'
        'forge later: whole = 42\norbit root_function() -> whole { yield later }\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 1
    assert "before initialization" in result.stderr
    assert f"{diagnostic_path(entry)}:3:" in result.stderr


@pytest.mark.parametrize("name", ["", "/absolute", "C:/drive", "C:drive", "//unc/path",
    "../outside", "a/../b", "./a", "a/./b", "a//b", "a/", "a.agne", "a-b",
    "2bad", "a/2bad", r"a\\b", r"a\0b", r"a\x00b", "a b", "é"])
def test_invalid_module_names_are_rejected(aphelion_executable, tmp_path, name):
    entry = module(tmp_path, "entry", f'summon "{name}"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1, result.stderr
    assert "invalid module path" in result.stderr


@pytest.mark.parametrize("source", [
    "summon\n", "summon thing\n", 'summon "valid" extra\n', 'summon "unterminated\n',
    'forge x = 1\nsummon "valid"\n', 'reveal(1)\nsummon "valid"\n',
    'orbit f() -> silent { summon "valid" }\n', '{ summon "valid" }\n',
    'when bright { summon "valid" }\n', 'cycle dim { summon "valid" }\n',
])
def test_imports_require_literal_top_level_preamble(aphelion_executable, tmp_path, source):
    module(tmp_path, "valid", "forge value = 1\n")
    entry = module(tmp_path, "entry", source)
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1, result.stderr
    assert "summon" in result.stderr or "expected" in result.stderr or "unterminated" in result.stderr


def test_missing_transitive_import_has_importing_filename(aphelion_executable, tmp_path):
    dependency = module(tmp_path, "nested/dep", '// first line\nsummon "missing"\n')
    entry = module(tmp_path, "entry", 'summon "nested/dep"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert f"{diagnostic_path(dependency)}:2:" in result.stderr, result.stderr
    assert "cannot open module source" in result.stderr
    assert "missing.agne" in result.stderr


@pytest.mark.parametrize("source", ['forge broken =\n', 'reveal("unterminated\n',
                                           'summon "missing"\n', 'reveal(1)\0garbage'])
def test_malformed_module_is_controlled(aphelion_executable, tmp_path, source):
    module(tmp_path, "dep", source)
    entry = module(tmp_path, "entry", 'summon "dep"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert result.stderr.strip()


@pytest.mark.parametrize("self_cycle", [False, True])
def test_import_cycle_is_a_located_error(aphelion_executable, tmp_path, self_cycle):
    entry = module(tmp_path, "entry", 'summon "entry"\n' if self_cycle else 'summon "dep"\n')
    dependency = module(tmp_path, "dep", 'summon "entry"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "cycle" in result.stderr
    assert diagnostic_path(entry) in result.stderr
    assert diagnostic_path(entry if self_cycle else dependency) in result.stderr


@pytest.mark.parametrize(("dependency", "root"), [
    ("forge same = 1\n", "forge same = 2\n"),
    ("orbit same() -> whole { yield 1 }\n", "orbit same() -> whole { yield 2 }\n"),
    ("forge same = 1\n", "orbit same() -> whole { yield 2 }\n"),
])
def test_shared_namespace_collisions_are_semantic_errors(aphelion_executable, tmp_path, dependency, root):
    module(tmp_path, "dep", dependency)
    entry = module(tmp_path, "entry", 'summon "dep"\n' + root)
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "duplicate name 'same'" in result.stderr


def test_tokens_does_not_resolve_imports(aphelion_executable, tmp_path):
    entry = module(tmp_path, "entry", 'summon "missing"\n')
    result = run_cli(aphelion_executable, "tokens", str(entry))
    assert result.returncode == 0, result.stderr
    assert "summon" in result.stdout


def symlink_or_skip(link, target, *, directory=False):
    try:
        link.symlink_to(target, target_is_directory=directory)
    except (OSError, NotImplementedError) as error:
        pytest.skip(f"symlinks unavailable: {error}")


@pytest.mark.parametrize("directory", [False, True])
def test_canonical_link_escape_and_root_prefix_boundary(aphelion_executable, tmp_path, directory):
    root = tmp_path / "root"
    outside = tmp_path / "root_extra"
    root.mkdir()
    outside.mkdir()
    target = module(outside, "payload", 'reveal("must not execute")\n')
    if directory:
        symlink_or_skip(root / "escape", outside, directory=True)
        name = "escape/payload"
    else:
        symlink_or_skip(root / "escape.agne", target)
        name = "escape"
    entry = module(root, "entry", f'summon "{name}"\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 1
    assert "escapes the canonical" in result.stderr
    assert result.stdout == ""


@pytest.mark.skipif(os.name != "nt", reason="Windows junction canonicalization")
def test_windows_junction_escape(aphelion_executable, tmp_path):
    root, outside = tmp_path / "root", tmp_path / "outside"
    root.mkdir()
    outside.mkdir()
    module(outside, "payload", 'reveal("outside")\n')
    junction = root / "linked"
    created = subprocess.run(["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
                             capture_output=True, text=True, timeout=5)
    if created.returncode != 0:
        pytest.skip(f"junction creation unavailable: {created.stderr}")
    try:
        entry = module(root, "entry", 'summon "linked/payload"\n')
        result = run_cli(aphelion_executable, "check", str(entry))
        assert result.returncode == 1
        assert "escapes the canonical" in result.stderr
    finally:
        junction.rmdir()


def test_canonical_alias_is_deduplicated(aphelion_executable, tmp_path):
    target = module(tmp_path, "original", 'reveal("once")\nforge exported = 3\n')
    symlink_or_skip(tmp_path / "alias.agne", target)
    entry = module(tmp_path, "entry", 'summon "original"\nsummon "alias"\nreveal(exported)\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "once\n3\n"


def test_hardlink_names_remain_distinct_modules(aphelion_executable, tmp_path):
    target = module(tmp_path, "original", "forge exported = 3\n")
    try:
        os.link(target, tmp_path / "alias.agne")
    except OSError as error:
        pytest.skip(f"hardlinks unavailable: {error}")
    entry = module(tmp_path, "entry", 'summon "original"\nsummon "alias"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "duplicate name 'exported'" in result.stderr


@pytest.mark.skipif(os.name != "nt", reason="Windows canonical identity is case insensitive")
def test_windows_case_insensitive_identity(aphelion_executable, tmp_path):
    module(tmp_path, "dep", 'reveal("once")\nforge value = 3\n')
    entry = module(tmp_path, "entry", 'summon "dep"\nsummon "DEP"\n')
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "once\n"


def test_depth_limit_and_boundary(aphelion_executable, tmp_path):
    for index in range(64):
        module(tmp_path, f"m{index}", f'summon "m{index + 1}"\n' if index < 63 else "reveal(64)\n")
    result = run_cli(aphelion_executable, "run", str(tmp_path / "m0.agne"))
    assert result.returncode == 0, result.stderr
    assert result.stdout == "64\n"
    module(tmp_path, "m63", 'summon "m64"\n')
    module(tmp_path, "m64", "")
    result = run_cli(aphelion_executable, "check", str(tmp_path / "m0.agne"))
    assert result.returncode == 1
    assert "depth limit exceeded (64)" in result.stderr


def test_graph_limit_and_boundary(aphelion_executable, tmp_path):
    for index in range(256):
        module(tmp_path, f"m{index}", "")
    entry = module(tmp_path, "entry", "".join(f'summon "m{i}"\n' for i in range(255)))
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 0, result.stderr
    with entry.open("a", encoding="utf-8") as stream:
        stream.write('summon "m255"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "graph limit exceeded (256)" in result.stderr


def test_import_size_is_bounded_before_reading(aphelion_executable, tmp_path):
    target = tmp_path / "large.agne"
    with target.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    entry = module(tmp_path, "entry", 'summon "large"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "aggregate 64 MiB limit" in result.stderr


@pytest.mark.parametrize("kind", ["directory", "fifo"])
def test_imports_reject_nonregular_files_without_blocking(aphelion_executable, tmp_path, kind):
    target = tmp_path / "special.agne"
    if kind == "directory":
        target.mkdir()
    elif hasattr(os, "mkfifo"):
        os.mkfifo(target)
    else:
        pytest.skip("FIFOs unavailable")
    entry = module(tmp_path, "entry", 'summon "special"\n')
    result = run_cli(aphelion_executable, "check", str(entry))
    assert result.returncode == 1
    assert "regular file" in result.stderr


def test_linked_apb_executes_after_entire_source_graph_is_deleted(
        aphelion_executable, source_executable, tmp_path):
    dependency = module(tmp_path, "dep", "forge value = 42\n")
    entry = module(tmp_path, "entry", 'summon "dep"\nreveal(value)\n')
    result = run_cli(aphelion_executable, "compile", str(entry))
    assert result.returncode == 0, result.stderr
    dependency.unlink()
    entry.unlink()
    result = subprocess.run([str(source_executable), str(entry.with_suffix(".apb"))],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "42\n"


@pytest.fixture(scope="session")
def module_checks(tmp_path_factory):
    build = tmp_path_factory.mktemp("module-checks")
    support = ROOT / "tests" / "support"
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("ast", "lexer", "token", "parser", "module"):
        output = build / (name + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
            "-c", str(ROOT / "src" / (name + ".c")), "-o", str(output)],
            capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("module-checks")
    result = subprocess.run([*flags, *objects, str(support / "module_checks.c"),
        str(support / "fail_alloc.c"), "-o", str(executable)],
        capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


@pytest.mark.parametrize("graph", ["diamond", "cycle", "missing", "malformed", "invalid"])
def test_loader_fault_injection_and_filename_lifetime(module_checks, tmp_path, graph):
    shared = "forge value = 1\n" if graph != "malformed" else "forge broken =\n"
    module(tmp_path, "shared", shared)
    module(tmp_path, "left", 'summon "shared"\n')
    module(tmp_path, "right", 'summon "shared"\n')
    if graph == "cycle":
        module(tmp_path, "shared", 'summon "entry"\n')
    elif graph == "missing":
        module(tmp_path, "shared", 'summon "missing"\n')
    elif graph == "invalid":
        module(tmp_path, "shared", 'summon "../outside"\n')
    entry = module(tmp_path, "entry", 'summon "left"\nsummon "right"\nreveal(value)\n')
    result = subprocess.run([str(module_checks), str(entry), "--sweep"],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == (0 if graph == "diamond" else 1), result.stderr
    assert "allocation cleanup checks passed" in result.stdout
