"""SOURCE validation, frame-local filenames, linked compilation and ownership."""

import struct
import subprocess

import pytest

from conftest import (ROOT, compiler_command, math_libraries, native_executable_name,
                      test_cflags as extra_cflags)
from test_aphelion import run_cli, write_program
from test_collection_vm import collection_executable, text, whole


SOURCE, MAKE_FUNCTION, CALL, RETURN, POP, HOLLOW = 45, 10, 29, 30, 4, 2


def source_apb(functions, constants=(), source=b"root.agne"):
    def string(value):
        return struct.pack("<I", len(value)) + value

    data = (b"APB1" + struct.pack("<5I", 1, 0, len(constants), len(functions), 0)
            + string(source) + b"".join(constants))
    for index, code in enumerate(functions):
        data += string(f"f{index}".encode()) + struct.pack("<3I", 0, 0, len(code))
        data += b"".join(struct.pack("<BiiII", *ins, 7, 9) for ins in code)
    return data


@pytest.fixture(scope="session")
def source_executable(tmp_path_factory):
    build = tmp_path_factory.mktemp("source-vm")
    support = ROOT / "tests" / "support"
    harness = build / "source_checks.c"
    harness.write_text(r'''
#define APHELION_ALLOC_IMPLEMENTATION
#include "fail_alloc.h"
#include "aphelion/parser.h"
#include "aphelion/compiler.h"
#include "aphelion/vm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(APHELION_OP_INDEX == 44 && APHELION_OP_SOURCE == 45,
               "old opcode numbers and appended SOURCE must remain stable");

static int pipeline(size_t failure, int unresolved) {
    Diagnostics diagnostics;
    AphelionDiagnostics errors;
    AstProgram *program;
    AphelionBytecode bytecode;
    AphelionVM vm;
    int ok = 0;
    size_t i, j, filenames = 0u;
    test_alloc_reset(failure);
    diagnostics_init(&diagnostics);
    aphelion_diagnostics_init(&errors);
    aphelion_bytecode_init(&bytecode);
    aphelion_vm_init(&vm);
    program = parser_parse("linked.agne",
        "orbit copy() -> whole { yield 4 }\nforge value = copy()\ncopy()\n",
        &diagnostics);
    if (program == NULL || program->had_parse_error || diagnostics_has_errors(&diagnostics))
        goto cleanup;
    program->source_files = test_calloc(1u, sizeof(*program->source_files));
    if (program->source_files == NULL) goto cleanup;
    program->source_file_capacity = 1u;
    program->source_files[0] = test_malloc(sizeof("linked.agne"));
    if (program->source_files[0] == NULL) goto cleanup;
    memcpy(program->source_files[0], "linked.agne", sizeof("linked.agne"));
    program->source_file_count = 1u;
    if (unresolved) {
        AstTopLevel item;
        memset(&item, 0, sizeof(item));
        item.kind = AST_TOP_EXPRESSION;
        item.as.statement = ast_stmt_new(AST_STMT_SUMMON, item.span);
        assert(item.as.statement != NULL);
        assert(ast_program_add_item(program, item));
    }
    ok = aphelion_compile_program(program, NULL, &bytecode, &errors);
    if (unresolved) {
        assert(!ok && aphelion_diagnostics_has_errors(&errors));
        for (i = 0u; i < errors.count; ++i)
            if (strstr(errors.items[i].message, "unresolved summon") != NULL) break;
        assert(i < errors.count);
        ok = 1;
        goto cleanup;
    }
    if (!ok) goto cleanup;
    for (i = 0u; i < bytecode.constant_count; ++i)
        if (bytecode.constants[i].type == APHELION_VALUE_TEXT &&
            strcmp(bytecode.constants[i].as.text->bytes, "linked.agne") == 0) ++filenames;
    assert(filenames == 1u);
    for (i = 0u; i < bytecode.function_count; ++i) {
        assert(bytecode.functions[i].code[0].opcode == APHELION_OP_SOURCE);
        for (j = 0u; j < bytecode.functions[i].code_count; ++j)
            if (bytecode.functions[i].code[j].opcode == APHELION_OP_SOURCE)
                assert(bytecode.functions[i].code[j].operand2 == 0);
    }
    /* The AST and its owned source paths may die before the bytecode executes. */
    ast_program_free(program);
    program = NULL;
    ok = aphelion_vm_run(&vm, &bytecode);
cleanup:
    aphelion_vm_destroy(&vm);
    aphelion_bytecode_destroy(&bytecode);
    ast_program_free(program);
    aphelion_diagnostics_destroy(&errors);
    diagnostics_free(&diagnostics);
    assert(test_alloc_live() == 0u);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--compile-sweep") == 0) {
        size_t point, attempts;
        assert(pipeline(0u, 0));
        attempts = test_alloc_attempts();
        for (point = 1u; point <= attempts; ++point) assert(!pipeline(point, 0));
        puts("linked compiler/VM allocation cleanup checks passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--unresolved") == 0) {
        assert(pipeline(0u, 1));
        return 0;
    }
    if (argc == 2) {
        AphelionBytecode bytecode;
        AphelionVM vm;
        AphelionRuntimeError saved;
        char error[256];
        int ok;
        test_alloc_reset(0u);
        aphelion_bytecode_init(&bytecode);
        aphelion_vm_init(&vm);
        ok = aphelion_bytecode_read_file(&bytecode, argv[1], error, sizeof(error));
        if (ok) {
            ok = aphelion_vm_run(&vm, &bytecode);
            saved = *aphelion_vm_error(&vm);
            aphelion_vm_destroy(&vm);
            if (!ok) fprintf(stderr, "%s:%u:%u: %s\n", saved.file,
                             (unsigned)saved.line, (unsigned)saved.column, saved.message);
        } else {
            fprintf(stderr, "%s\n", error);
            aphelion_vm_destroy(&vm);
        }
        aphelion_bytecode_destroy(&bytecode);
        assert(test_alloc_live() == 0u);
        return ok ? 0 : 1;
    }
    return 2;
}
''', encoding="utf-8")
    flags = [*compiler_command(), "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *extra_cflags(), f"-I{ROOT / 'include'}", f"-I{support}"]
    objects = []
    for name in ("ast", "token", "lexer", "parser", "compiler", "file",
                 "value", "bytecode", "vm"):
        output = build / (name + ".o")
        result = subprocess.run([*flags, "-include", str(support / "fail_alloc.h"),
                                 "-c", str(ROOT / "src" / (name + ".c")), "-o", str(output)],
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stderr
        objects.append(str(output))
    executable = build / native_executable_name("source-checks")
    result = subprocess.run([*flags, *objects, str(harness), str(support / "fail_alloc.c"),
                             *math_libraries(), "-o", str(executable)],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return executable


def run_source(source_executable, tmp_path, functions, constants=()):
    path = tmp_path / "source.apb"
    path.write_bytes(source_apb(functions, constants))
    return subprocess.run([str(source_executable), str(path)], capture_output=True,
                          text=True, timeout=5)


@pytest.mark.parametrize("constant", [b"\x00", whole(1), b"\x02" + struct.pack("<d", 1),
                                       b"\x03\x01", b"\x05" + struct.pack("<I", 65),
                                       b"\x06" + struct.pack("<I", 0), b"\x07",
                                       text(b""), text(b"\0"), text(b"a\0b"), text(b"a\0")])
def test_source_rejects_nontext_empty_and_nul_constants(source_executable, tmp_path, constant):
    result = run_source(source_executable, tmp_path, [[(SOURCE, 0, 0), (0, 0, 0)]], [constant])
    assert result.returncode == 1, result.stderr
    assert "SOURCE" in result.stderr


@pytest.mark.parametrize(("operand", "operand2"), [(-2147483648, 0), (-1, 0), (1, 0),
                                                      (2147483647, 0), (0, 1), (0, -1)])
def test_source_rejects_bad_ranges_and_unused_operand(source_executable, tmp_path, operand, operand2):
    result = run_source(source_executable, tmp_path, [[(SOURCE, operand, operand2), (0, 0, 0)]],
                        [text(b"dep.agne")])
    assert result.returncode == 1, result.stderr
    assert "operand" in result.stderr


@pytest.mark.parametrize(("where", "expected"), [(0, "root.agne"), (1, "dep.agne"),
                                                (2, "nested/leaf-é.agne")])
def test_nested_calls_restore_each_callers_marker(source_executable, tmp_path, where, expected):
    functions = []
    names = ["root.agne", "dep.agne", "nested/leaf-é.agne"]
    for index in range(3):
        code = [(SOURCE, index, 0)]
        if index < 2:
            code += [(MAKE_FUNCTION, index + 1, 0), (CALL, 0, 0), (POP, 0, 0)]
        if index == where:
            code += [(POP, 0, 0)]  # Empty operand stack at the failing frame.
        code += [(HOLLOW, 0, 0), (RETURN, 0, 0)]
        functions.append(code)
    result = run_source(source_executable, tmp_path, functions,
                        [text(name.encode()) for name in names])
    assert result.returncode == 1
    assert result.stderr == f"{expected}:7:9: operand stack underflow\n"


def test_unmarked_callee_defaults_to_bytecode_source(source_executable, tmp_path):
    result = run_source(source_executable, tmp_path, [
        [(SOURCE, 0, 0), (MAKE_FUNCTION, 1, 0), (CALL, 0, 0), (0, 0, 0)],
        [(POP, 0, 0)]], [text(b"dep.agne")])
    assert result.returncode == 1
    assert result.stderr == "root.agne:7:9: operand stack underflow\n"


@pytest.mark.parametrize("where", [0, 20, 39])
def test_source_markers_survive_frame_array_growth(source_executable, tmp_path, where):
    functions = []
    for index in range(40):
        code = [(SOURCE, index, 0)]
        if index < 39:
            code += [(MAKE_FUNCTION, index + 1, 0), (CALL, 0, 0), (POP, 0, 0)]
        if index == where:
            code += [(POP, 0, 0)]
        functions.append(code + [(HOLLOW, 0, 0), (RETURN, 0, 0)])
    result = run_source(source_executable, tmp_path, functions,
                        [text(f"module{i}.agne".encode()) for i in range(40)])
    assert result.returncode == 1
    assert result.stderr == f"module{where}.agne:7:9: operand stack underflow\n"


def test_source_updates_current_frame_at_statement_boundary(source_executable, tmp_path):
    result = run_source(source_executable, tmp_path,
                        [[(SOURCE, 0, 0), (SOURCE, 1, 0), (POP, 0, 0)]],
                        [text(b"dep.agne"), text(b"root.agne")])
    assert result.returncode == 1
    assert result.stderr == "root.agne:7:9: operand stack underflow\n"


@pytest.mark.parametrize("mode", ["--compile-sweep", "--unresolved"])
def test_linked_compiler_cleanup_and_unresolved_diagnostic(source_executable, mode):
    result = subprocess.run([str(source_executable), mode], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("fails", [False, True])
def test_source_bytecode_roundtrip_and_vm_oom_cleanup(collection_executable, tmp_path, fails):
    path, rewritten = tmp_path / "linked.apb", tmp_path / "rewritten.apb"
    path.write_bytes(source_apb([
        [(SOURCE, 0, 0), (MAKE_FUNCTION, 1, 0), (CALL, 0, 0), (POP, 0, 0), (0, 0, 0)],
        [(SOURCE, 1, 0)] + ([(POP, 0, 0)] if fails else [])
        + [(HOLLOW, 0, 0), (RETURN, 0, 0)]],
        [text(b"root.agne"), text(b"dep.agne")]))
    result = subprocess.run([str(collection_executable), str(path), str(rewritten)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == int(fails), result.stderr
    assert rewritten.read_bytes() == path.read_bytes()
    result = subprocess.run([str(collection_executable), "--sweep", str(rewritten)],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr


def test_single_file_compiler_has_no_source_markers(aphelion_executable, tmp_path):
    path = write_program(tmp_path, "single.agne", "orbit f() -> whole { yield 4 }\nreveal(f())\n")
    result = run_cli(aphelion_executable, "compile", str(path))
    assert result.returncode == 0, result.stderr
    result = run_cli(aphelion_executable, "disasm", str(path.with_suffix(".apb")))
    assert result.returncode == 0, result.stderr
    assert "SOURCE" not in result.stdout


@pytest.mark.parametrize(("module", "root", "filename", "line"), [
    ('forge bad = 1 / 0\n', 'reveal(1)\n', "dep.agne", 1),
    ('forge good = 3\n', 'reveal(1 / 0)\n', "root.agne", 2),
    ('orbit fail() -> whole { yield 1 / 0 }\n', 'reveal(fail())\n', "dep.agne", 1),
    ('orbit pass() -> whole { yield rootcall() }\n',
     'orbit rootcall() -> whole { yield 1 / 0 }\nreveal(pass())\n', "root.agne", 2),
    ('orbit pass() -> whole { yield rootcall() }\n',
     'orbit rootcall() -> whole { yield 3 }\nreveal(pass())\nreveal(1 / 0)\n', "root.agne", 4),
    ('orbit pass() -> whole { yield rootcall() / 0 }\n',
     'orbit rootcall() -> whole { yield 3 }\nreveal(pass())\n', "dep.agne", 1),
])
def test_linked_runtime_filenames_and_standalone_reexecution(
    aphelion_executable, source_executable, tmp_path, module, root, filename, line
):
    dependency = tmp_path / "dep.agne"
    dependency.write_text(module, encoding="utf-8")
    entry = tmp_path / "root.agne"
    entry.write_text('summon "dep"\n' + root, encoding="utf-8")
    compiled = run_cli(aphelion_executable, "compile", str(entry))
    assert compiled.returncode == 0, compiled.stderr
    assert entry.with_suffix(".apb").read_bytes()[:8] == b"APB1\x01\x00\x00\x00"
    result = run_cli(aphelion_executable, "run", str(entry))
    assert result.returncode == 1
    expected = f"{(tmp_path / filename).resolve()}:{line}:"
    assert expected in result.stderr, result.stderr
    dependency.unlink()
    entry.unlink()
    result = subprocess.run([str(source_executable), str(entry.with_suffix(".apb"))],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 1
    assert expected in result.stderr, result.stderr
    assert "division by zero" in result.stderr
