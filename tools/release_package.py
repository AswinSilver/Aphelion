#!/usr/bin/env python3
"""Build a verified, reproducible Aphelion release archive.

The executable is intentionally an input to this command.  The command never
builds or locates a different executable, and it inspects the input's native
imports before writing an archive.

Examples::

    python tools/release_package.py windows --version 0.1.0 \
        --executable .\\aphelion.exe --output dist\\aphelion-0.1.0-windows.zip
    python tools/release_package.py linux --version 0.1.0 \
        --executable ./aphelion --output dist/aphelion-0.1.0-linux.tar.gz
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from typing import Iterable, NoReturn, Sequence


ROOT = Path(__file__).resolve().parents[1]
PRODUCT = "aphelion"
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
REQUIRED_EXAMPLES = (
    "hello.agne",
    "factorial.agne",
    "control_flow.agne",
    "collections.agne",
    "builtins_practical.agne",
    "builtins.agne",
    "traverse.agne",
    "repl_lines.agne",
    "modules.agne",
)
REQUIRED_MODULES = ("lib/math.agne",)
SYSTEM_API_SET_PREFIXES = ("api-ms-win-", "ext-ms-win-")


class PackageFailure(Exception):
    """A release precondition or packaging operation failed."""


@dataclass(frozen=True)
class Dependency:
    name: str
    resolved: Path | None
    system: bool
    source: str


@dataclass(frozen=True)
class Artifact:
    source: Path
    archive_path: str
    kind: str


def fail(message: str) -> NoReturn:
    raise PackageFailure(message)


def required_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        fail(f"required dependency inspection tool is missing from PATH: {name!r}")
    return path


def run_tool(command: Sequence[str], *, label: str) -> str:
    try:
        result = subprocess.run(
            list(command), capture_output=True, text=True, encoding="utf-8",
            errors="replace", check=False,
        )
    except OSError as error:
        fail(f"could not run {label}: {error}")
    if result.returncode != 0:
        details = (result.stderr or result.stdout).strip()
        suffix = f": {details}" if details else ""
        fail(f"{label} failed with status {result.returncode}{suffix}")
    return result.stdout


def checked_relative(path: str, *, root: Path, description: str) -> Path:
    candidate = (root / path).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        fail(f"{description} is outside the project root: {path!r}")
    return candidate


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        fail(f"cannot read artifact {path}: {error}")
    return digest.hexdigest()


def normalize_archive_path(path: str) -> str:
    normalized = str(PurePosixPath(path.replace("\\", "/")))
    if normalized in ("", ".") or normalized.startswith("../") or "/../" in normalized:
        fail(f"invalid archive member path: {path!r}")
    return normalized


def require_executable(path: Path) -> Path:
    executable = path.expanduser().resolve()
    if not executable.is_file():
        fail(f"executable is not a file: {executable}")
    return executable


def verify_version(executable: Path, version: str) -> None:
    expected = f"{PRODUCT} {version}\n"
    try:
        result = subprocess.run(
            [str(executable), "--version"], capture_output=True,
            text=True, encoding="utf-8", errors="replace", check=False,
            timeout=5,
        )
    except subprocess.TimeoutExpired as error:
        fail(f"executable identity check timed out: {executable} ({error})")
    except OSError as error:
        fail(f"could not execute artifact for identity check: {executable}: {error}")
    if result.returncode != 0:
        fail(
            f"wrong artifact identity: {executable} --version exited "
            f"with status {result.returncode}; stdout={result.stdout!r}; "
            f"stderr={result.stderr!r}"
        )
    if result.stdout != expected or result.stderr:
        fail(
            f"wrong artifact identity: expected exact output {expected!r}; "
            f"got stdout={result.stdout!r}, stderr={result.stderr!r}"
        )


def inspect_file_identity(executable: Path, mode: str, file_tool: str) -> str:
    output = run_tool([file_tool, str(executable)], label="file")
    first_line = output.strip().splitlines()[0] if output.strip() else ""
    if mode == "windows":
        if ("PE32+" not in first_line or "x86-64" not in first_line or
                "executable" not in first_line.lower()):
            fail(f"unsupported Windows x64 artifact identity: {first_line or output!r}")
    else:
        if ("ELF" not in first_line or "64-bit" not in first_line or
                ("x86-64" not in first_line and "x86_64" not in first_line) or
                "executable" not in first_line.lower()):
            fail(f"unsupported Linux x64 artifact identity: {first_line or output!r}")
    return first_line


def parse_windows_imports(output: str) -> list[str]:
    names = []
    for line in output.splitlines():
        match = re.search(r"\bDLL Name:\s*(\S+)", line, flags=re.IGNORECASE)
        if match and match.group(1).lower() not in {name.lower() for name in names}:
            names.append(match.group(1))
    return names


def windows_path(value: str) -> Path:
    """Make a PATH/SystemRoot entry usable by native and MSYS Python."""
    path = Path(value)
    if path.exists() or os.name == "nt":
        return path
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", value)
    if match:
        return Path("/") / match.group(1).lower() / match.group(2).replace("\\", "/")
    return path


def path_key(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


def find_case_insensitive(directory: Path, name: str) -> Path | None:
    exact = directory / name
    if exact.is_file():
        return exact.resolve()
    if not directory.is_dir():
        return None
    wanted = name.casefold()
    try:
        for entry in directory.iterdir():
            if entry.is_file() and entry.name.casefold() == wanted:
                return entry.resolve()
    except OSError:
        return None
    return None


def windows_system_roots() -> list[Path]:
    roots: list[Path] = []
    for value in (os.environ.get("SystemRoot"), os.environ.get("WINDIR")):
        if value:
            root = windows_path(value)
            roots.extend((root / "System32", root / "SysWOW64"))
    # MSYS2 may expose Windows directories even when SystemRoot is absent.
    for value in ("/c/Windows/System32", "/c/windows/System32"):
        candidate = Path(value)
        if candidate.exists():
            roots.append(candidate)
    return [root.resolve() for root in roots if root.is_dir()]


def is_under(path: Path, directories: Iterable[Path]) -> bool:
    resolved = path.resolve()
    return any(resolved == directory or directory in resolved.parents for directory in directories)


def windows_search_dirs(executable: Path) -> list[Path]:
    directories = [executable.parent]
    for value in os.environ.get("PATH", "").split(os.pathsep):
        if value:
            directory = windows_path(value)
            if directory not in directories:
                directories.append(directory)
    return directories


def resolve_windows(name: str, directories: Sequence[Path]) -> Path | None:
    for directory in directories:
        resolved = find_case_insensitive(directory, name)
        if resolved is not None:
            return resolved
    return None


def inspect_windows(executable: Path, objdump: str) -> list[Dependency]:
    system_roots = windows_system_roots()
    search_dirs = windows_search_dirs(executable)
    dependencies: list[Dependency] = []
    visited: set[str] = set()
    pending: list[tuple[str, Path]] = [(PRODUCT, executable)]
    while pending:
        source_name, source_path = pending.pop(0)
        source_key = path_key(source_path)
        if source_key in visited:
            continue
        visited.add(source_key)
        imports = parse_windows_imports(
            run_tool([objdump, "-p", str(source_path)], label=f"objdump ({source_path.name})")
        )
        for name in imports:
            lower_name = name.casefold()
            # API-set DLLs are Windows loader contracts. They are not ordinary
            # files and are supplied by the target OS, so there is no file to
            # copy; this is identified from the import namespace, not a list of
            # guessed runtime DLL names.
            api_set = lower_name.startswith(SYSTEM_API_SET_PREFIXES)
            resolved = None if api_set else resolve_windows(name, [source_path.parent, *search_dirs])
            system = api_set or (resolved is not None and is_under(resolved, system_roots))
            if resolved is None and not system:
                fail(
                    f"unresolved non-system Windows dependency {name!r} "
                    f"imported by {source_path} (searched executable directory and PATH)"
                )
            dependencies.append(Dependency(name, resolved, system, str(source_path)))
            if resolved is not None and not system:
                pending.append((name, resolved))
    unique: dict[tuple[str, str], Dependency] = {}
    for dependency in dependencies:
        key = (dependency.name.casefold(), path_key(dependency.resolved) if dependency.resolved else "system")
        unique[key] = dependency
    return sorted(unique.values(), key=lambda item: (item.system, item.name.casefold()))


def linux_system_roots() -> list[Path]:
    roots = [Path("/lib"), Path("/lib32"), Path("/lib64"), Path("/usr/lib"),
             Path("/usr/lib32"), Path("/usr/lib64"), Path("/usr/local/lib")]
    try:
        import sysconfig
        libdir = sysconfig.get_config_var("LIBDIR")
        if libdir:
            roots.append(Path(libdir))
    except (ImportError, OSError):
        pass
    return [root.resolve() for root in roots if root.is_dir()]


def parse_ldd(output: str) -> list[tuple[str, Path | None, bool]]:
    dependencies: list[tuple[str, Path | None, bool]] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "=> not found" in line:
            name = line.split("=>", 1)[0].strip()
            dependencies.append((name, None, False))
            continue
        if "=>" in line:
            name, rest = (part.strip() for part in line.split("=>", 1))
            path_text = rest.split("(", 1)[0].strip()
            path = Path(path_text) if path_text.startswith("/") else None
            dependencies.append((name, path, False))
            continue
        if line.startswith("linux-vdso"):
            dependencies.append((line.split(None, 1)[0], None, True))
            continue
        # The ELF interpreter is printed without an arrow by ldd.
        if line.startswith("/"):
            path_text = line.split("(", 1)[0].strip()
            path = Path(path_text)
            dependencies.append((path.name, path, False))
    return dependencies


def inspect_linux(executable: Path, ldd: str) -> list[Dependency]:
    try:
        result = subprocess.run(
            [ldd, str(executable)], capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=False,
        )
    except OSError as error:
        fail(f"could not run ldd: {error}")
    output = result.stdout
    if result.returncode != 0 and "not a dynamic executable" not in output.lower():
        missing = [line.split("=>", 1)[0].strip() for line in output.splitlines()
                   if "=> not found" in line]
        if missing:
            fail("unresolved non-system Linux dependencies reported by ldd: " + ", ".join(missing))
        details = (result.stderr or output).strip()
        suffix = f": {details}" if details else ""
        fail(f"ldd failed with status {result.returncode}{suffix}")
    if "not a dynamic executable" in output.lower():
        parsed: list[tuple[str, Path | None, bool]] = []
    else:
        parsed = parse_ldd(output)
    system_roots = linux_system_roots()
    dependencies: list[Dependency] = []
    for name, resolved, virtual_system in parsed:
        if resolved is None and not virtual_system:
            fail(f"unresolved non-system Linux dependency {name!r} reported by ldd")
        system = virtual_system or (resolved is not None and is_under(resolved, system_roots))
        if resolved is not None and not resolved.is_file():
            fail(f"ldd resolved dependency {name!r} to a missing file: {resolved}")
        dependencies.append(Dependency(name, resolved, system, str(executable)))
    return sorted(dependencies, key=lambda item: (item.system, item.name))


def select_sources(root: Path, includes: Sequence[str]) -> list[Artifact]:
    examples_root = root / "examples"
    if not examples_root.is_dir():
        fail(f"required examples directory is missing: {examples_root}")
    if includes:
        paths = []
        for value in includes:
            # Accept the concise `hello.agne` spelling as well as a repository
            # relative `examples/hello.agne` spelling in CI arguments.
            relative_value = value.replace("\\", "/")
            if relative_value == "examples" or relative_value.startswith("examples/"):
                paths.append(checked_relative(value, root=root, description="included example/module"))
            else:
                paths.append(checked_relative(value, root=examples_root,
                                              description="included example/module"))
    else:
        missing = [examples_root / value for value in (*REQUIRED_EXAMPLES, *REQUIRED_MODULES)
                   if not (examples_root / value).is_file()]
        if missing:
            fail("required examples/modules are missing: " + ", ".join(str(path) for path in missing))
        paths = sorted(path for path in examples_root.rglob("*") if path.is_file() and path.suffix == ".agne")
    artifacts: list[Artifact] = []
    seen: set[str] = set()
    for path in paths:
        if not path.is_file():
            fail(f"selected example/module is missing: {path}")
        try:
            relative = path.relative_to(examples_root)
        except ValueError:
            fail(f"selected example/module is outside examples/: {path}")
        archive_path = normalize_archive_path(f"examples/{relative.as_posix()}")
        if archive_path in seen:
            fail(f"selected example/module is listed more than once: {archive_path}")
        seen.add(archive_path)
        artifacts.append(Artifact(path, archive_path, "example/module"))
    if not artifacts:
        fail("no examples/modules were selected")
    return sorted(artifacts, key=lambda item: item.archive_path)


def dependency_artifacts(dependencies: Sequence[Dependency]) -> list[Artifact]:
    artifacts: list[Artifact] = []
    seen: dict[str, Path] = {}
    for dependency in dependencies:
        if dependency.system or dependency.resolved is None:
            continue
        source = dependency.resolved
        # Keep runtime files beside the executable.  This is the native DLL
        # search location on Windows. Linux currently rejects private shared
        # libraries until a verified RPATH/launcher is part of the package.
        archive_path = normalize_archive_path(source.name)
        previous = seen.get(archive_path)
        if previous is not None and path_key(previous) != path_key(source):
            fail(
                f"different non-system dependencies would collide in the archive "
                f"as {archive_path!r}: {previous} and {source}"
            )
        if previous is None:
            seen[archive_path] = source
            artifacts.append(Artifact(source, archive_path, "runtime dependency"))
    return sorted(artifacts, key=lambda item: item.archive_path.casefold())


def make_manifest(version: str, mode: str, identity: str, executable: Artifact,
                  artifacts: Sequence[Artifact], dependencies: Sequence[Dependency],
                  hashes: dict[str, str]) -> tuple[bytes, bytes]:
    files = [
        {"path": item.archive_path, "kind": item.kind, "size": item.source.stat().st_size,
         "sha256": hashes[item.archive_path]}
        for item in sorted(artifacts, key=lambda item: item.archive_path)
    ]
    manifest = {
        "format": 1,
        "product": PRODUCT,
        "version": version,
        "platform": mode,
        "native_identity": identity,
        "dependency_inspector": "objdump -p" if mode == "windows" else "ldd",
        "executable": executable.archive_path,
        "executable_sha256": hashes[executable.archive_path],
        # Keep machine-local build paths out of the archive.  The manifest is
        # intended to be audited and compared across runners, so dependency
        # records use names/classifications rather than absolute paths.
        "dependencies": [
            {"name": item.name,
             "classification": "system" if item.system else "bundled",
             "archive_path": item.resolved.name if item.resolved is not None and not item.system else None,
             "imported_by": Path(item.source).name}
            for item in dependencies
        ],
        "files": files,
        "checksums": "SHA256SUMS",
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    checksum_lines = [
        f"{hashes[item.archive_path]}  {item.archive_path}\n"
        for item in sorted(artifacts, key=lambda item: item.archive_path)
    ]
    # SHA256SUMS cannot contain its own hash without becoming recursive, but
    # it can and should cover the manifest alongside every payload file.
    checksum_lines.append(
        f"{hashlib.sha256(manifest_bytes).hexdigest()}  manifest.json\n"
    )
    checksum_bytes = "".join(checksum_lines).encode("utf-8")
    return manifest_bytes, checksum_bytes


def zip_info(name: str, source: Path) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 0
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = ((0o755 if os.access(source, os.X_OK) else 0o644) & 0xFFFF) << 16
    return info


def write_zip(output: Path, members: dict[str, bytes | Path]) -> None:
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(members):
            value = members[name]
            if isinstance(value, Path):
                with value.open("rb") as stream:
                    archive.writestr(zip_info(name, value), stream.read())
            else:
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 0
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, value)


def write_tar_gz(output: Path, members: dict[str, bytes | Path]) -> None:
    with output.open("wb") as raw:
        import gzip
        # Suppress the output filename in the gzip header.  Otherwise two
        # equivalent archives written to different paths would hash
        # differently even though every tar member is identical.
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name in sorted(members):
                    value = members[name]
                    info = tarfile.TarInfo(name)
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    if isinstance(value, Path):
                        info.size = value.stat().st_size
                        info.mode = 0o755 if os.access(value, os.X_OK) else 0o644
                        with value.open("rb") as stream:
                            archive.addfile(info, stream)
                    else:
                        info.size = len(value)
                        info.mode = 0o644
                        archive.addfile(info, io.BytesIO(value))


def member_hash(value: bytes | Path) -> str:
    if isinstance(value, Path):
        return sha256(value)
    return hashlib.sha256(value).hexdigest()


def verify_archive(output: Path, mode: str,
                   expected: dict[str, str]) -> None:
    """Reopen the archive and verify every member written by the packager."""
    try:
        if mode == "windows":
            with zipfile.ZipFile(output, "r") as archive:
                names = archive.namelist()
                if len(names) != len(set(names)) or set(names) != set(expected):
                    fail("written Windows archive has unexpected or duplicate members")
                for name, digest in expected.items():
                    if hashlib.sha256(archive.read(name)).hexdigest() != digest:
                        fail(f"written Windows archive member failed checksum verification: {name}")
                if archive.testzip() is not None:
                    fail("written Windows archive contains a corrupt member")
        else:
            with tarfile.open(output, "r:gz") as archive:
                members = archive.getmembers()
                names = [member.name for member in members]
                if (len(names) != len(set(names)) or set(names) != set(expected) or
                        any(not member.isfile() for member in members)):
                    fail("written Linux archive has unexpected, duplicate, or non-file members")
                for name, digest in expected.items():
                    stream = archive.extractfile(name)
                    if stream is None or hashlib.sha256(stream.read()).hexdigest() != digest:
                        fail(f"written Linux archive member failed checksum verification: {name}")
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        fail(f"could not verify written archive {output}: {error}")


def package(mode: str, version: str, output: Path, executable: Path,
            root: Path, includes: Sequence[str], force: bool) -> Path:
    if not VERSION_PATTERN.fullmatch(version):
        fail(f"invalid version {version!r}; expected a dotted version such as 0.1.0")
    executable = require_executable(executable)
    expected_executable_name = "aphelion.exe" if mode == "windows" else "aphelion"
    if executable.name.casefold() != expected_executable_name.casefold():
        fail(
            f"release executable must be named {expected_executable_name!r}; "
            f"got {executable.name!r}"
        )
    root = root.resolve()
    if not root.is_dir():
        fail(f"project root is not a directory: {root}")
    output = output.expanduser().resolve()
    expected_suffix = ".zip" if mode == "windows" else ".tar.gz"
    if not str(output).lower().endswith(expected_suffix):
        fail(f"{mode} mode requires an output ending in {expected_suffix}: {output}")
    if output.exists() and not force:
        fail(f"output already exists (use --force to replace it): {output}")
    if output == executable:
        fail("output archive must not overwrite the executable")

    file_tool = required_tool("file")
    identity = inspect_file_identity(executable, mode, file_tool)
    verify_version(executable, version)
    if mode == "windows":
        dependencies = inspect_windows(executable, required_tool("objdump"))
    else:
        dependencies = inspect_linux(executable, required_tool("ldd"))

    root_name = f"{PRODUCT}-{version}-{mode}"
    executable_name = executable.name
    executable_artifact = Artifact(executable, executable_name, "native executable")
    source_artifacts = select_sources(root, includes)
    runtime_artifacts = dependency_artifacts(dependencies)
    if mode == "linux" and runtime_artifacts:
        fail(
            "Linux artifact has non-system shared-library dependencies; "
            "packaging requires a verified RPATH/launcher before those files "
            "can be distributed safely"
        )
    artifacts = [executable_artifact, *runtime_artifacts, *source_artifacts]
    archive_paths = [item.archive_path for item in artifacts]
    if len(archive_paths) != len(set(archive_paths)):
        fail("executable and runtime artifacts have colliding archive names")
    archive_members: dict[str, bytes | Path] = {
        f"{root_name}/{item.archive_path}": item.source for item in artifacts
    }
    hashes = {item.archive_path: sha256(item.source) for item in artifacts}
    manifest, checksums = make_manifest(version, mode, identity, executable_artifact,
                                       artifacts, dependencies, hashes)
    archive_members[f"{root_name}/manifest.json"] = manifest
    archive_members[f"{root_name}/SHA256SUMS"] = checksums
    expected_member_hashes = {
        name: member_hash(value) for name, value in archive_members.items()
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp",
                                         dir=output.parent, delete=False) as temporary:
            temporary_path = Path(temporary.name)
        if mode == "windows":
            write_zip(temporary_path, archive_members)
        else:
            write_tar_gz(temporary_path, archive_members)
        if output.exists() and force:
            output.unlink()
        temporary_path.replace(output)
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        try:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        fail(f"could not write archive {output}: {error}")

    verify_archive(output, mode, expected_member_hashes)

    sidecar = output.with_name(output.name + ".sha256")
    sidecar.write_text(f"{sha256(output)}  {output.name}\n", encoding="ascii", newline="\n")
    print(f"Release package created: {output}")
    print(f"Archive SHA-256: {sha256(output)}")
    print(f"Included files: {len(artifacts)}; non-system runtime files: {len(runtime_artifacts)}")
    print(f"Archive manifest: {root_name}/manifest.json")
    print(f"Archive checksums: {root_name}/SHA256SUMS")
    print(f"Archive checksum sidecar: {sidecar}")
    return output


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("windows", "linux"), help="native archive format and dependency inspector")
    parser.add_argument("--version", required=True, help="release version, for example 0.1.0")
    parser.add_argument("--output", required=True, type=Path, help="destination .zip or .tar.gz archive")
    parser.add_argument("--executable", required=True, type=Path, help="exact prebuilt native executable path")
    parser.add_argument("--project-root", type=Path, default=ROOT, help="project root containing examples/ (default: repository root)")
    parser.add_argument("--include", action="append", default=[], metavar="EXAMPLE_OR_MODULE",
                        help="selected path under examples/; repeat to override the default release selection")
    parser.add_argument("--force", action="store_true", help="replace an existing archive and checksum sidecar")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        package(args.mode, args.version, args.output, args.executable,
                args.project_root, args.include, args.force)
    except PackageFailure as error:
        print(f"Release package FAILED: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"Release package FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
