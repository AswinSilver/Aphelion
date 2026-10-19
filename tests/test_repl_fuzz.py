"""Run the existing bounded seeded source corpus through the real REPL too."""

import importlib.util
import subprocess
import sys

from conftest import ROOT


def test_seeded_source_corpus_is_controlled_in_repl(aphelion_executable):
    name = "aphelion_repl_fuzz_corpus"
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / "fuzz_harness.py")
    corpus = importlib.util.module_from_spec(spec)
    sys.modules[name] = corpus
    try:
        spec.loader.exec_module(corpus)
        cases = corpus.generate_source_cases(
            2026, 64, max_case_bytes=8192, max_total_bytes=1024 * 1024
        )
        assert len(cases) == 64
        for index, case in enumerate(cases):
            result = subprocess.run(
                [str(aphelion_executable), "repl"], input=case.payload + b"\n",
                capture_output=True, timeout=5,
            )
            assert result.returncode in (0, 1), (
                index, case.category, result.returncode, result.stderr[-2000:]
            )
    finally:
        sys.modules.pop(name, None)
