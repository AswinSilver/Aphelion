"""Regression tests for the verified release packager."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import subprocess
import tarfile
import zipfile

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "release_package.py"
SPEC = importlib.util.spec_from_file_location("release_package", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
release_package = importlib.util.module_from_spec(SPEC)
sys.modules["release_package"] = release_package
SPEC.loader.exec_module(release_package)
WORKFLOW_PATH = ROOT / ".github" / "workflows" / "release-package.yml"


def windows_powershell() -> str | None:
    candidates = [shutil.which("powershell.exe"), shutil.which("pwsh")]
    if sys.platform == "win32":
        candidates.append(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")
    return next((candidate for candidate in candidates if candidate and Path(candidate).is_file()), None)


def extract_windows_validator() -> str:
    lines = WORKFLOW_PATH.read_text(encoding="utf-8").splitlines(True)
    marker = "      - name: Verify and run the extracted Windows package\n"
    start = lines.index(marker)
    run_line = start + 1
    while lines[run_line].strip() != "run: |":
        run_line += 1
    body = []
    cursor = run_line + 1
    while cursor < len(lines) and (lines[cursor].startswith("          ") or not lines[cursor].strip()):
        body.append(lines[cursor][10:] if lines[cursor].startswith("          ") else lines[cursor])
        cursor += 1
    return "".join(body).replace("${{ inputs.version }}", "0.1.0")


def make_minimal_windows_package(path: Path, external_attributes: int,
                                create_system: int) -> None:
    root = "aphelion-0.1.0-windows"
    executable = b"not reached"
    executable_hash = hashlib.sha256(executable).hexdigest()
    manifest = json.dumps({
        "format": 1,
        "product": "aphelion",
        "version": "0.1.0",
        "platform": "windows",
        "native_identity": "PE32+ executable x86-64",
        "dependency_inspector": "objdump -p",
        "executable": "aphelion.exe",
        "executable_sha256": executable_hash,
        "dependencies": [],
        "files": [{"path": "aphelion.exe", "kind": "native executable",
                   "size": len(executable), "sha256": executable_hash}],
        "checksums": "SHA256SUMS",
    }, sort_keys=True, indent=2).encode("utf-8") + b"\n"
    checksums = (
        f"{executable_hash}  aphelion.exe\n"
        f"{hashlib.sha256(manifest).hexdigest()}  manifest.json\n"
    ).encode("ascii")
    with zipfile.ZipFile(path, "w") as archive:
        executable_info = zipfile.ZipInfo(f"{root}/aphelion.exe")
        executable_info.create_system = create_system
        executable_info.external_attr = external_attributes
        archive.writestr(executable_info, executable)
        archive.writestr(f"{root}/manifest.json", manifest)
        archive.writestr(f"{root}/SHA256SUMS", checksums)


@pytest.mark.parametrize(
    ("label", "external_attributes", "create_system"),
    [
        ("DOS", 0x0010, 0),
        ("Unix", 0x4000 << 16, 3),
    ],
)
def test_windows_validator_rejects_directory_metadata_before_extraction(
    tmp_path: Path, label: str, external_attributes: int, create_system: int,
) -> None:
    powershell = windows_powershell()
    if powershell is None:
        pytest.skip("PowerShell is unavailable")

    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    archive = downloaded / "aphelion-0.1.0-windows.zip"
    make_minimal_windows_package(archive, external_attributes, create_system)
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    (downloaded / (archive.name + ".sha256")).write_text(
        f"{archive_hash}  {archive.name}\n", encoding="ascii"
    )
    script = tmp_path / "validate.ps1"
    script.write_text(extract_windows_validator(), encoding="utf-8")

    result = subprocess.run(
        [powershell, "-NoProfile", "-NonInteractive", "-File", str(script)],
        cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", check=False,
    )
    output = (result.stdout or "") + (result.stderr or "")
    assert result.returncode != 0, f"{label} directory metadata was accepted:\n{output}"
    assert "directory member" in output
    assert not (tmp_path / "installed").exists()
    assert "packaged Windows" not in output


def make_examples(root: Path) -> Path:
    examples = root / "examples"
    (examples / "lib").mkdir(parents=True)
    for name in (*release_package.REQUIRED_EXAMPLES, *release_package.REQUIRED_MODULES):
        path = examples / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode("ascii"))
    return examples


def fake_file_and_linux_tools(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_required_tool(name: str) -> str:
        return name

    def fake_run_tool(command, *, label: str) -> str:
        if label == "file":
            assert command[1] == "-b"
            return "ELF 64-bit LSB pie executable, x86-64\n"
        raise AssertionError(f"unexpected tool invocation: {command!r}")

    monkeypatch.setattr(release_package, "required_tool", fake_required_tool)
    monkeypatch.setattr(release_package, "run_tool", fake_run_tool)
    monkeypatch.setattr(release_package, "verify_version", lambda *args: None)
    monkeypatch.setattr(release_package, "inspect_linux", lambda *args: [])


def test_identical_bytes_from_different_paths_make_identical_archives(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake_file_and_linux_tools(monkeypatch)
    first_root = tmp_path / "checkout one"
    second_root = tmp_path / "a different checkout"
    make_examples(first_root)
    make_examples(second_root)
    first_executable = first_root / "aphelion"
    second_executable = second_root / "aphelion"
    executable_bytes = b"same executable bytes\x00\xff\n"
    first_executable.write_bytes(executable_bytes)
    second_executable.write_bytes(executable_bytes)
    first_output = tmp_path / "output one" / "package.tar.gz"
    second_output = tmp_path / "output two" / "package.tar.gz"

    release_package.package(
        "linux", "1.2.3", first_output, first_executable, first_root,
        ["hello.agne"], False,
    )
    release_package.package(
        "linux", "1.2.3", second_output, second_executable, second_root,
        ["hello.agne"], False,
    )

    assert first_output.read_bytes() == second_output.read_bytes()
    assert hashlib.sha256(first_output.read_bytes()).digest() == hashlib.sha256(
        second_output.read_bytes()
    ).digest()
    with tarfile.open(first_output, "r:gz") as archive:
        manifest = json.loads(archive.extractfile("aphelion-1.2.3-linux/manifest.json").read())
        members = {member.name: archive.extractfile(member).read() for member in archive.getmembers()}
    assert manifest["native_identity"] == "ELF 64-bit LSB pie executable, x86-64"
    assert str(first_root) not in json.dumps(manifest)
    assert str(second_root) not in json.dumps(manifest)
    assert all(not Path(value).is_absolute() for value in (
        item["path"] for item in manifest["files"]
    ))
    assert all(str(first_root) not in value and str(second_root) not in value
               for value in members)


def fake_objdump(monkeypatch: pytest.MonkeyPatch, graph: dict[str, list[str]]) -> None:
    def fake_run_tool(command, *, label: str) -> str:
        assert label.startswith("objdump")
        source = Path(command[-1]).name.casefold()
        return "\n".join(f"\tDLL Name: {name}" for name in graph.get(source, []))

    monkeypatch.setattr(release_package, "run_tool", fake_run_tool)


def windows_fixture(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    windows = tmp_path / "Windows"
    system32 = windows / "System32"
    system32.mkdir(parents=True)
    private = tmp_path / "private"
    private.mkdir()
    executable = tmp_path / "app" / "aphelion.exe"
    executable.parent.mkdir()
    executable.write_bytes(b"exe")
    monkeypatch.setenv("SystemRoot", str(windows))
    monkeypatch.delenv("WINDIR", raising=False)
    monkeypatch.setenv("PATH", str(private))
    return executable, system32, private


def test_windows_rejects_direct_forbidden_runtime_import(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    executable, _system32, _private = windows_fixture(tmp_path, monkeypatch)
    fake_objdump(monkeypatch, {"aphelion.exe": ["cygwin1.dll"]})

    with pytest.raises(release_package.PackageFailure, match="forbidden MSYS/Cygwin"):
        release_package.inspect_windows(executable, "objdump")


def test_windows_rejects_transitive_forbidden_runtime_import(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    executable, _system32, private = windows_fixture(tmp_path, monkeypatch)
    (private / "private.dll").write_bytes(b"private")
    fake_objdump(monkeypatch, {
        "aphelion.exe": ["private.dll"],
        "private.dll": ["msys-2.0.dll"],
    })

    with pytest.raises(release_package.PackageFailure, match="forbidden MSYS/Cygwin"):
        release_package.inspect_windows(executable, "objdump")


def test_windows_rejects_unresolved_non_system_import(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    executable, _system32, _private = windows_fixture(tmp_path, monkeypatch)
    fake_objdump(monkeypatch, {"aphelion.exe": ["missing.dll"]})

    with pytest.raises(release_package.PackageFailure, match="unresolved non-system"):
        release_package.inspect_windows(executable, "objdump")


def test_windows_system_root_wins_over_same_named_path_dll(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    executable, system32, private = windows_fixture(tmp_path, monkeypatch)
    system_dll = system32 / "shared.dll"
    private_dll = private / "shared.dll"
    system_dll.write_bytes(b"system")
    private_dll.write_bytes(b"private")
    fake_objdump(monkeypatch, {"aphelion.exe": ["shared.dll"]})

    dependencies = release_package.inspect_windows(executable, "objdump")

    assert len(dependencies) == 1
    assert dependencies[0].system is True
    assert dependencies[0].resolved == system_dll.resolve()
    assert release_package.dependency_artifacts(dependencies) == []


def test_default_examples_reject_non_regular_files(tmp_path: Path) -> None:
    examples = make_examples(tmp_path)
    (examples / "not-a-file.agne").mkdir()

    with pytest.raises(release_package.PackageFailure, match="not a regular file"):
        release_package.select_sources(tmp_path, [])


@pytest.mark.parametrize("target_kind", ["inside", "outside"])
def test_default_examples_reject_symlinks(tmp_path: Path, target_kind: str) -> None:
    examples = make_examples(tmp_path)
    outside = tmp_path / "outside.agne"
    outside.write_bytes(b"outside")
    target = examples / "hello.agne" if target_kind == "inside" else outside
    link = examples / f"{target_kind}-link.agne"
    try:
        link.symlink_to(target)
    except (OSError, NotImplementedError) as error:
        pytest.skip(f"symlinks unavailable: {error}")

    with pytest.raises(release_package.PackageFailure, match="must not be a symlink"):
        release_package.select_sources(tmp_path, [])
