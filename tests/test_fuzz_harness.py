"""Bounded regression tests for the deterministic fuzz harness."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tools" / "fuzz_harness.py"
INCLUDE = ROOT / "include"
SOURCES = sorted((ROOT / "src").glob("*.c"))


def run_harness(executable: Path, mode: str, *options: str) -> tuple[subprocess.CompletedProcess[str], dict]:
    command = [sys.executable, str(HARNESS), mode, "--executable", str(executable), *options]
    result = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=90,
    )
    report = json.loads(result.stdout)
    return result, report


def test_source_fuzzing_is_deterministic_and_covers_edge_corpus(
    strict_aphelion_executable: Path,
) -> None:
    options = ("--cases", "18", "--seed", "2026", "--timeout", "2", "--max-total-bytes", "200000")
    first_result, first = run_harness(strict_aphelion_executable, "source", *options)
    second_result, second = run_harness(strict_aphelion_executable, "source", *options)

    assert first_result.returncode == 0, first_result.stderr
    assert second_result.returncode == 0, second_result.stderr
    assert first == second
    assert first["passed"] is True
    assert "sanitizer-error" in first["checks"]["failures"]
    assert first["executed_cases"] == 18
    assert first["total_bytes"] <= 200000
    assert all(case["size"] <= 64 * 1024 for case in first["cases"])

    categories = {case["category"] for case in first["cases"]}
    assert {
        "empty",
        "random-bytes",
        "random-text",
        "deeply-nested-expression",
        "long-string",
        "invalid-escape",
        "unterminated-comment",
        "unterminated-string",
        "unterminated-glyph",
        "malformed-number",
        "extreme-whole-literal",
        "extreme-decimal-literal",
    } <= categories
    assert all("source_b64" in case for case in first["cases"])


def test_bytecode_fuzzing_allows_expected_diagnostics(
    strict_aphelion_executable: Path,
    tmp_path: Path,
) -> None:
    report_path = tmp_path / "bytecode-report.json"
    result, report = run_harness(
        strict_aphelion_executable,
        "bytecode",
        "--cases",
        "16",
        "--seed",
        "2026",
        "--timeout",
        "2",
        "--max-total-bytes",
        "200000",
        "--report",
        str(report_path),
    )

    assert result.returncode == 0, result.stderr
    assert report["passed"] is True
    assert report_path.read_text(encoding="utf-8") == result.stdout
    assert report["executed_cases"] == 16
    assert report["total_bytes"] <= 200000
    categories = {case["category"] for case in report["cases"]}
    assert {
        "valid-baseline",
        "bad-magic",
        "bad-version",
        "corrupt-counts",
        "truncation",
        "unknown-opcode",
        "bad-operands",
        "bad-jump-target",
        "trailing-bytes",
    } <= categories
    assert all("bytecode_b64" in case for case in report["cases"])
    assert any(case["status"] == "diagnostic" for case in report["cases"])


def test_size_bound_can_limit_cases_without_changing_the_seeded_report_shape(
    strict_aphelion_executable: Path,
) -> None:
    result, report = run_harness(
        strict_aphelion_executable,
        "source",
        "--cases",
        "40",
        "--seed",
        "7",
        "--timeout",
        "2",
        "--max-case-bytes",
        "100",
        "--max-total-bytes",
        "200",
    )

    assert result.returncode == 0, result.stderr
    assert report["requested_cases"] == 40
    assert report["executed_cases"] <= 40
    assert report["skipped_due_to_size"] == 40 - report["executed_cases"]
    assert report["total_bytes"] <= 200
    assert all(case["size"] <= 100 for case in report["cases"])
