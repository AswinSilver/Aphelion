"""Focused checks for the portable release command itself."""

from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SMOKE = ROOT / "tools" / "release_smoke.py"


def run_smoke(
    executable: Path, directory: Path, version: str | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(SMOKE)]
    if version is not None:
        command.extend(["--version", version])
    command.extend(["--executable", str(executable)])
    return subprocess.run(
        command,
        cwd=directory, capture_output=True, text=True, encoding="utf-8",
        timeout=120, check=False,
    )


def test_release_from_unrelated_directory_leaves_sources_untouched(
    aphelion_executable: Path, tmp_path: Path,
) -> None:
    before = {path: path.read_bytes() for path in (ROOT / "examples").iterdir() if path.is_file()}
    directory = tmp_path / "working directory with spaces"
    directory.mkdir()
    result = run_smoke(aphelion_executable, directory)
    assert result.returncode == 0, result.stderr
    assert "Release smoke passed:" in result.stdout
    assert result.stderr == ""
    assert list(directory.iterdir()) == []
    after = {path: path.read_bytes() for path in (ROOT / "examples").iterdir() if path.is_file()}
    assert after == before


def test_missing_exact_path_is_not_replaced_by_another_binary(tmp_path: Path) -> None:
    result = run_smoke(tmp_path / "missing executable", tmp_path)
    assert result.returncode == 2
    assert "executable is not a file" in result.stderr
    assert "Release smoke passed" not in result.stdout


def test_wrong_native_binary_fails_with_captured_output(tmp_path: Path) -> None:
    result = run_smoke(Path(sys.executable), tmp_path)
    assert result.returncode == 1
    assert "Release smoke FAILED" in result.stderr
    assert "expected stdout" in result.stderr
    assert "Python " in result.stderr
    assert "Release smoke passed" not in result.stdout


def test_release_input_must_match_checked_in_version(
    aphelion_executable: Path, tmp_path: Path,
) -> None:
    result = run_smoke(aphelion_executable, tmp_path, version="9.9.9")
    assert result.returncode == 1
    assert "version mismatch" in result.stderr


def test_timeout_reports_failure_and_cleans_temporary_files(tmp_path, monkeypatch, capsys):
    command = runpy.run_path(str(SMOKE))

    def timeout(args, **kwargs):
        raise subprocess.TimeoutExpired(args, kwargs["timeout"])

    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    monkeypatch.setattr(subprocess, "run", timeout)
    assert command["main"](["--executable", sys.executable]) == 1
    assert "timed out" in capsys.readouterr().err
    assert list(tmp_path.iterdir()) == []
