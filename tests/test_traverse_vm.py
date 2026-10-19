"""Compiled traversal bytecode, bounded execution, and retained-value cleanup."""

import struct
import subprocess

import pytest

from test_aphelion import run_cli, write_program
from test_collection_vm import collection_executable
from test_core_checks import core_executable


def compile_traversal(aphelion_executable, tmp_path, source):
    path = write_program(tmp_path, "traversal.agne", source)
    result = run_cli(aphelion_executable, "compile", str(path))
    assert result.returncode == 0, result.stderr
    bytecode = path.with_suffix(".apb")
    assert bytecode.read_bytes()[:8] == b"APB1\x01\x00\x00\x00"
    return bytecode


def test_compiled_traversal_roundtrip(core_executable, aphelion_executable, tmp_path):
    bytecode = compile_traversal(aphelion_executable, tmp_path, r'''
        shift calls = 0
        orbit make() -> [text] { calls += 1; yield ["a\0tail", "skip", "c"] }
        orbit visit(xs: [text]) -> text {
            traverse item in xs {
                when item == "skip" { skip }
                traverse item in [item] { reveal(item); escape }
            }
            traverse item in xs { yield item }
            yield "empty"
        }
        shift source = make()
        traverse source in source { reveal(source) }
        traverse item in source {
            source = ["replacement"]
            when item == "skip" { skip }
            reveal(item)
        }
        reveal(visit(make()))
        reveal(calls)
        reveal(source[0])
    ''')
    rewritten = tmp_path / "rewritten.apb"
    result = subprocess.run([str(core_executable), str(bytecode), str(rewritten)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    expected = "a\0tail\nskip\nc\na\0tail\nc\na\0tail\nc\na\0tail\n2\nreplacement\n"
    assert result.stdout == expected
    assert rewritten.read_bytes() == bytecode.read_bytes()
    rerun = subprocess.run([str(core_executable), str(rewritten)],
                           capture_output=True, text=True, timeout=5)
    assert rerun.returncode == 0, rerun.stderr
    assert rerun.stdout == expected
    disasm = run_cli(aphelion_executable, "disasm", str(rewritten))
    assert disasm.returncode == 0, disasm.stderr
    for opcode in ("LIST", "INDEX", "LENGTH", "ADD", "JUMP", "STORE_LOCAL"):
        assert opcode in disasm.stdout
    loaded = subprocess.run([str(core_executable), "--alloc-read", str(bytecode)],
                            capture_output=True, text=True, timeout=5)
    assert loaded.returncode == 0, loaded.stderr


def probe_entry_locals(bytecode):
    """Observe compiler-private slots after loop exit, before frame teardown.

    Replace the entry epilogue with slot-kind probes followed by that same
    epilogue. Existing jump targets remain valid because no earlier code moves.
    """
    data = bytearray(bytecode.read_bytes())
    entry, constants, functions, globals_ = struct.unpack_from("<4I", data, 8)
    assert entry == 0 and functions == 1
    offset = 24

    def skip_string(position):
        return position + 4 + struct.unpack_from("<I", data, position)[0]

    offset = skip_string(offset)
    for _ in range(constants):
        tag = data[offset]
        offset += 1
        if tag == 4:
            offset = skip_string(offset)
        else:
            offset += {0: 0, 1: 8, 2: 8, 3: 1, 5: 4, 6: 4, 7: 0}[tag]
    for _ in range(globals_):
        offset = skip_string(offset)
    offset = skip_string(offset)  # entry function name
    _, local_count, code_count = struct.unpack_from("<3I", data, offset)
    code_start = offset + 12
    epilogue = code_start + (code_count - 3) * 17
    assert [data[epilogue + i * 17] for i in range(3)] == [2, 30, 0]
    probes = []
    for slot in range(local_count):
        probes += [(6, slot, 0), (41, 0, 0), (31, 0, 0), (4, 0, 0)]
    encoded = b"".join(struct.pack("<BiiII", *ins, 1, 1) for ins in probes)
    struct.pack_into("<I", data, offset + 8, code_count + len(probes))
    bytecode.write_bytes(data[:epilogue] + encoded + data[epilogue:])
    return local_count


@pytest.mark.parametrize("source", [
    'traverse item in ["owned"] { reveal(item) }',
    'traverse item in ["owned"] { escape }',
    'traverse item in ["owned"] { skip }',
    'forge xs: [text] = []\ntraverse item in xs { reveal(item) }',
    'traverse item in ["owned"] { traverse item in [item] { escape }; escape }',
])
def test_loop_private_slots_are_hollow_before_entry_returns(
    core_executable, aphelion_executable, tmp_path, source
):
    bytecode = compile_traversal(aphelion_executable, tmp_path, source)
    count = probe_entry_locals(bytecode)
    assert count in (3, 6)
    result = subprocess.run([str(core_executable), str(bytecode)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[-count:] == ["hollow"] * count


@pytest.mark.parametrize(("body", "limited"), [
    ('traverse item in ["owned"] { skip }', False),
    ('traverse item in ["owned"] { cycle bright { skip } }', True),
    ('traverse item in ["owned"] { cycle bright { escape }; skip }', False),
    ('cycle bright { traverse item in ["owned"] { skip } }', True),
])
@pytest.mark.parametrize("in_function", [False, True])
def test_compiled_loops_obey_controlled_instruction_budget(
    core_executable, aphelion_executable, tmp_path, body, limited, in_function
):
    # core_checks fixes the budget at 1000 instructions and checks its allocation
    # ledger after VM teardown on both successful and budget-error paths.
    source = (f"orbit work() -> silent {{ {body}\n yield }}\nwork()"
              if in_function else body)
    bytecode = compile_traversal(aphelion_executable, tmp_path, source)
    result = subprocess.run([str(core_executable), str(bytecode)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == int(limited), result.stderr
    if limited:
        assert "instruction limit" in result.stderr
    else:
        assert result.stderr == ""


@pytest.mark.parametrize("tail", ["escape", "skip", "yield item", "assert(dim, item)"])
def test_compiled_traversal_allocation_failure_sweep(
    collection_executable, aphelion_executable, tmp_path, tail
):
    bytecode = compile_traversal(aphelion_executable, tmp_path, f'''
        orbit visit(xs: [text]) -> text {{
            traverse item in xs {{
                traverse inner in [item + "owned"] {{ reveal(inner) }}
                {tail}
            }}
            yield "done"
        }}
        reveal(visit(["first\\0tail", "second"]))
    ''')
    # Existing runtime-only sweep fails each loader/VM allocation in turn and
    # checks every retained list/text is freed, including return/error exits.
    result = subprocess.run([str(collection_executable), "--sweep", str(bytecode)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert "VM allocation cleanup checks passed" in result.stdout
