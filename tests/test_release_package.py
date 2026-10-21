"""Regression tests for the verified release packager."""

from __future__ import annotations

import hashlib
import io
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
VERIFY_MODULE_PATH = ROOT / "tools" / "verify_release_archive.py"
VERIFY_SPEC = importlib.util.spec_from_file_location("verify_release_archive", VERIFY_MODULE_PATH)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
verify_release_archive = importlib.util.module_from_spec(VERIFY_SPEC)
sys.modules["verify_release_archive"] = verify_release_archive
VERIFY_SPEC.loader.exec_module(verify_release_archive)
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
    root.mkdir(parents=True, exist_ok=True)
    (root / "LICENSE").write_bytes(b"MIT License\n")
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
    assert members["aphelion-1.2.3-linux/LICENSE"] == b"MIT License\n"
    assert {item["path"] for item in manifest["files"]} >= {"LICENSE", "aphelion"}


def test_package_rejects_version_different_from_project_source(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = tmp_path / "project"
    make_examples(root)
    source = root / "include" / "aphelion"
    source.mkdir(parents=True)
    (source / "version.h").write_text(
        '#define APHELION_VERSION "0.1.0"\n', encoding="utf-8"
    )
    executable = root / "aphelion"
    executable.write_bytes(b"executable")
    with pytest.raises(release_package.PackageFailure, match="version mismatch"):
        release_package.package(
            "linux", "1.2.3", tmp_path / "package.tar.gz", executable, root,
            ["hello.agne"], False,
        )


def test_package_requires_repository_license(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake_file_and_linux_tools(monkeypatch)
    root = tmp_path / "project"
    make_examples(root)
    (root / "LICENSE").unlink()
    executable = root / "aphelion"
    executable.write_bytes(b"executable")

    with pytest.raises(release_package.PackageFailure, match="MIT license file"):
        release_package.package(
            "linux", "1.2.3", tmp_path / "package.tar.gz", executable, root,
            ["hello.agne"], False,
        )


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


def test_explicit_examples_reject_non_agne_files(tmp_path: Path) -> None:
    examples = make_examples(tmp_path)
    extra = examples / "not-source.txt"
    extra.write_text("must not ship", encoding="utf-8")

    with pytest.raises(release_package.PackageFailure, match=".agne extension"):
        release_package.select_sources(tmp_path, ["not-source.txt"])


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


def make_verified_archive(path: Path, mode: str, *, executable_attributes: int = 0,
                          executable_name: str | None = None,
                          manifest_changes: dict | None = None,
                          payload: bytes = b"native executable",
                          extra_payload: dict[str, bytes] | None = None,
                          include_license: bool = True) -> None:
    """Write a small archive in the public release format for validator tests."""
    root = f"aphelion-0.1.0-{mode}"
    executable = "aphelion.exe" if mode == "windows" else "aphelion"
    executable = executable_name or executable
    executable_hash = hashlib.sha256(payload).hexdigest()
    payloads = {"aphelion.exe" if mode == "windows" else "aphelion": payload}
    if include_license:
        payloads["LICENSE"] = b"MIT License\n"
    payloads.update(extra_payload or {})
    files = []
    for relative, data in sorted(payloads.items()):
        kind = ("native executable" if relative == ("aphelion.exe" if mode == "windows" else "aphelion")
                else "license" if relative == "LICENSE"
                else "runtime dependency" if mode == "windows" and relative.casefold().endswith(".dll")
                else "example/module")
        files.append({"path": relative, "kind": kind, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()})
    manifest = {
        "format": 1,
        "product": "aphelion",
        "version": "0.1.0",
        "platform": mode,
        "native_identity": "PE32+ executable x86-64" if mode == "windows"
        else "ELF 64-bit LSB pie executable, x86-64",
        "dependency_inspector": "objdump -p" if mode == "windows" else "ldd",
        "executable": "aphelion.exe" if mode == "windows" else "aphelion",
        "executable_sha256": executable_hash,
        "dependencies": [],
        "files": files,
        "checksums": "SHA256SUMS",
    }
    if manifest_changes:
        manifest.update(manifest_changes)
    manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode("utf-8")
    checksum_lines = [
        f"{hashlib.sha256(data).hexdigest()}  {relative}\n"
        for relative, data in sorted(payloads.items())
    ]
    checksum_lines.append(f"{hashlib.sha256(manifest_bytes).hexdigest()}  manifest.json\n")
    checksums = "".join(checksum_lines).encode("ascii")
    if mode == "windows":
        with zipfile.ZipFile(path, "w") as archive:
            for relative, data in sorted(payloads.items()):
                info = zipfile.ZipInfo(f"{root}/{relative}")
                info.external_attr = executable_attributes if relative == executable else 0
                archive.writestr(info, data)
            archive.writestr(f"{root}/manifest.json", manifest_bytes)
            archive.writestr(f"{root}/SHA256SUMS", checksums)
    else:
        with tarfile.open(path, "w:gz") as archive:
            tar_payloads = [
                (f"{root}/{relative}", data, 0o755 if relative == executable else 0o644)
                for relative, data in sorted(payloads.items())
            ]
            for name, data, mode_bits in (*tar_payloads,
                                          (f"{root}/manifest.json", manifest_bytes, 0o644),
                                          (f"{root}/SHA256SUMS", checksums, 0o644)):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = mode_bits
                archive.addfile(info, io.BytesIO(data))


@pytest.mark.parametrize("mode", ["windows", "linux"])
def test_release_archive_validator_accepts_minimal_public_packages(
    tmp_path: Path, mode: str,
) -> None:
    archive = tmp_path / ("package.zip" if mode == "windows" else "package.tar.gz")
    make_verified_archive(archive, mode)

    verify_release_archive.validate_archive(archive, mode, "0.1.0")


@pytest.mark.parametrize(
    ("mode", "extra_payload"),
    [("windows", {"ucrtbase.dll": b"runtime"}),
     ("linux", {"examples/hello.agne": b"hello"})],
)
def test_release_archive_validator_accepts_platform_payload_members(
    tmp_path: Path, mode: str, extra_payload: dict[str, bytes],
) -> None:
    archive = tmp_path / ("package.zip" if mode == "windows" else "package.tar.gz")
    make_verified_archive(archive, mode, extra_payload=extra_payload)

    verify_release_archive.validate_archive(archive, mode, "0.1.0")


@pytest.mark.parametrize("mode", ["windows", "linux"])
def test_release_archive_validator_requires_license(
    tmp_path: Path, mode: str,
) -> None:
    archive = tmp_path / ("package.zip" if mode == "windows" else "package.tar.gz")
    make_verified_archive(archive, mode, include_license=False)

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="required LICENSE"):
        verify_release_archive.validate_archive(archive, mode, "0.1.0")


@pytest.mark.parametrize("external_attributes", [0x0010, 0x4000 << 16])
def test_release_archive_validator_rejects_windows_directory_bits(
    tmp_path: Path, external_attributes: int,
) -> None:
    archive = tmp_path / "package.zip"
    make_verified_archive(archive, "windows", executable_attributes=external_attributes)

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="directory member"):
        verify_release_archive.validate_archive(archive, "windows", "0.1.0")


def test_release_archive_validator_rejects_traversal_before_manifest_read(tmp_path: Path) -> None:
    archive = tmp_path / "package.zip"
    root = "aphelion-0.1.0-windows"
    with zipfile.ZipFile(archive, "w") as output:
        output.writestr(f"{root}/../outside", b"unsafe")

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="unsafe archive member path"):
        verify_release_archive.validate_archive(archive, "windows", "0.1.0")


def test_release_archive_validator_rejects_manifest_hash_mismatch(tmp_path: Path) -> None:
    archive = tmp_path / "package.tar.gz"
    make_verified_archive(archive, "linux", payload=b"changed after manifest")
    # The fixture's manifest was generated from this payload, so corrupt only
    # the executable member while preserving the archive's metadata files.
    rewritten = tmp_path / "corrupt.tar.gz"
    with tarfile.open(archive, "r:gz") as source, tarfile.open(rewritten, "w:gz") as output:
        for member in source.getmembers():
            data = source.extractfile(member).read()
            if member.name.endswith("/aphelion"):
                data = b"X" * len(data)
                member.size = len(data)
            output.addfile(member, io.BytesIO(data))

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="manifest file hash mismatch"):
        verify_release_archive.validate_archive(rewritten, "linux", "0.1.0")


def test_release_archive_validator_rejects_wrong_platform_metadata(tmp_path: Path) -> None:
    archive = tmp_path / "package.zip"
    make_verified_archive(archive, "windows", manifest_changes={"platform": "linux"})

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="wrong release metadata"):
        verify_release_archive.validate_archive(archive, "windows", "0.1.0")


def test_release_archive_validator_rejects_executable_sha256_mismatch(tmp_path: Path) -> None:
    archive = tmp_path / "package.zip"
    make_verified_archive(archive, "windows", manifest_changes={"executable_sha256": "0" * 64})

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="executable_sha256 mismatch"):
        verify_release_archive.validate_archive(archive, "windows", "0.1.0")


def test_release_archive_validator_rejects_inconsistent_sha256sums(tmp_path: Path) -> None:
    archive = tmp_path / "package.zip"
    make_verified_archive(archive, "windows")
    corrupt = tmp_path / "corrupt.zip"
    with zipfile.ZipFile(archive, "r") as source, zipfile.ZipFile(corrupt, "w") as output:
        for info in source.infolist():
            data = source.read(info)
            if info.filename.endswith("/SHA256SUMS"):
                data = ("0" * 64 + "  aphelion.exe\n"
                        + data.decode("ascii").split("\n", 1)[1]).encode("ascii")
            output.writestr(info, data)

    with pytest.raises(verify_release_archive.ArchiveValidationError, match="SHA256SUMS hash mismatch"):
        verify_release_archive.validate_archive(corrupt, "windows", "0.1.0")
