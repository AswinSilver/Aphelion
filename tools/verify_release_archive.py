#!/usr/bin/env python3
"""Validate an Aphelion release archive without extracting it.

The release archive is treated as untrusted input.  This checker reads ZIP and
tar members directly, validates their metadata and names, and only then reads
regular-file contents for the manifest and checksum checks.  It never calls an
archive extraction operation or writes files to disk.

Examples::

    python tools/verify_release_archive.py windows \
        --archive dist/aphelion-0.1.0-windows.zip --version 0.1.0
    python tools/verify_release_archive.py linux \
        --archive dist/aphelion-0.1.0-linux.tar.gz --version 0.1.0
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tarfile
from typing import Any, NoReturn
import zipfile


PRODUCT = "aphelion"
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
CHECKSUM_LINE = re.compile(r"^([0-9a-fA-F]{64})  (.+)$")
DRIVE_PATH = re.compile(r"^[A-Za-z]:")


class ArchiveValidationError(Exception):
    """Raised when an archive is unsafe or does not match its release metadata."""


def fail(message: str) -> NoReturn:
    raise ArchiveValidationError(message)


def _safe_name(name: str, *, label: str) -> str:
    """Validate an archive-relative POSIX path before it is used."""
    if (not isinstance(name, str) or not name or "\x00" in name or "\\" in name
            or name.startswith("/") or DRIVE_PATH.match(name)):
        fail(f"{label} has an unsafe archive member path: {name!r}")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts):
        fail(f"{label} has an unsafe archive member path: {name!r}")
    return name


def _check_root(name: str, root: str, *, label: str) -> None:
    parts = name.split("/")
    if len(parts) < 2 or parts[0] != root:
        fail(f"{label} has an unexpected top-level root: {name!r}")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _zip_members(path: Path, root: str) -> dict[str, bytes]:
    members: dict[str, bytes] = {}
    folded_names: set[str] = set()
    try:
        with zipfile.ZipFile(path, "r") as archive:
            infos = archive.infolist()
            if not infos:
                fail("Windows archive is empty")
            for info in infos:
                name = _safe_name(info.filename, label="Windows archive member")
                _check_root(name, root, label="Windows archive member")

                # ZIP stores DOS attributes in the low word and, when a Unix
                # producer wrote the entry, the Unix mode in the high word.
                attributes = info.external_attr & 0xFFFFFFFF
                unix_type = (attributes >> 16) & 0xF000
                if info.is_dir() or (attributes & 0x0010) or unix_type == 0x4000:
                    fail(f"Windows archive contains a directory member: {name}")
                if unix_type == 0xA000 or (attributes & 0x0400):
                    fail(f"Windows archive contains a symlink or reparse member: {name}")
                if unix_type not in (0, 0x8000):
                    fail(f"Windows archive contains a non-regular member: {name}")

                folded = name.casefold()
                if name in members or folded in folded_names:
                    fail(f"Windows archive contains a duplicate member: {name}")
                try:
                    data = archive.read(info)
                except (OSError, RuntimeError, zipfile.BadZipFile) as error:
                    fail(f"Windows archive member cannot be read: {name}: {error}")
                members[name] = data
                folded_names.add(folded)
    except ArchiveValidationError:
        raise
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        fail(f"could not read Windows ZIP archive {path}: {error}")
    return members


def _tar_members(path: Path, root: str) -> dict[str, bytes]:
    members: dict[str, bytes] = {}
    try:
        with tarfile.open(path, "r:gz") as archive:
            entries = archive.getmembers()
            if not entries:
                fail("Linux archive is empty")
            for entry in entries:
                name = _safe_name(entry.name, label="Linux archive member")
                _check_root(name, root, label="Linux archive member")

                # TarInfo.type is the authoritative Unix file type.  The mode
                # check also catches deliberately forged 0x4000/0xA000 modes.
                unix_type = entry.mode & 0xF000
                if entry.isdir() or unix_type == 0x4000:
                    fail(f"Linux archive contains a directory member: {name}")
                if entry.issym() or entry.islnk() or unix_type == 0xA000:
                    fail(f"Linux archive contains a symlink or hardlink member: {name}")
                if entry.type not in (tarfile.REGTYPE, tarfile.AREGTYPE) or not entry.isfile():
                    fail(f"Linux archive contains a non-regular member: {name}")

                if name in members:
                    fail(f"Linux archive contains a duplicate member: {name}")
                stream = archive.extractfile(entry)
                if stream is None:
                    fail(f"Linux archive member cannot be read: {name}")
                try:
                    members[name] = stream.read()
                except OSError as error:
                    fail(f"Linux archive member cannot be read: {name}: {error}")
    except ArchiveValidationError:
        raise
    except (OSError, tarfile.TarError) as error:
        fail(f"could not read Linux tar.gz archive {path}: {error}")
    return members


def _json_no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _parse_manifest(data: bytes, mode: str, version: str) -> dict[str, Any]:
    try:
        text = data.decode("utf-8")
        manifest = json.loads(
            text,
            object_pairs_hook=_json_no_duplicate_keys,
            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        fail(f"manifest.json is malformed: {error}")
    if not isinstance(manifest, dict):
        fail("manifest.json is malformed: top level must be an object")

    expected_inspector = "objdump -p" if mode == "windows" else "ldd"
    required = {
        "format", "product", "version", "platform", "native_identity",
        "dependency_inspector", "executable", "executable_sha256",
        "dependencies", "files", "checksums",
    }
    if not required.issubset(manifest):
        missing = sorted(required - set(manifest))
        fail(f"manifest.json is malformed: missing fields {missing}")
    if (type(manifest["format"]) is not int or manifest["format"] != 1
            or manifest["product"] != PRODUCT or manifest["version"] != version
            or manifest["platform"] != mode or manifest["checksums"] != "SHA256SUMS"
            or manifest["dependency_inspector"] != expected_inspector):
        fail("manifest.json has wrong release metadata")

    expected_executable = "aphelion.exe" if mode == "windows" else "aphelion"
    if manifest["executable"] != expected_executable:
        fail(f"manifest.json has wrong executable metadata: {manifest['executable']!r}")
    identity = manifest["native_identity"]
    if not isinstance(identity, str) or not identity:
        fail("manifest.json has malformed native_identity metadata")
    identity_lower = identity.casefold()
    if mode == "windows":
        valid_identity = "pe32+" in identity_lower and "x86-64" in identity_lower and "executable" in identity_lower
    else:
        valid_identity = ("elf" in identity_lower and "64-bit" in identity_lower
                          and ("x86-64" in identity_lower or "x86_64" in identity_lower)
                          and "executable" in identity_lower)
    if not valid_identity:
        fail("manifest.json has wrong native executable identity")

    if not isinstance(manifest["dependencies"], list):
        fail("manifest.json has malformed dependencies")
    for dependency in manifest["dependencies"]:
        if not isinstance(dependency, dict):
            fail("manifest.json has a malformed dependency entry")
        if not isinstance(dependency.get("name"), str) or not dependency["name"]:
            fail("manifest.json has a malformed dependency name")
        if dependency.get("classification") not in ("system", "bundled"):
            fail("manifest.json has a malformed dependency classification")
        archive_path = dependency.get("archive_path")
        if archive_path is not None:
            _safe_name(archive_path, label="manifest dependency")
            if "/" in archive_path:
                fail("manifest dependency archive_path must be a file name")
        if not isinstance(dependency.get("imported_by"), str) or not dependency["imported_by"]:
            fail("manifest.json has a malformed dependency importer")

    if not isinstance(manifest["files"], list):
        fail("manifest.json has malformed files")
    executable_hash = manifest["executable_sha256"]
    if not isinstance(executable_hash, str) or not SHA256_PATTERN.fullmatch(executable_hash):
        fail("manifest.json has a malformed executable_sha256")
    return manifest


def _allowed_payload_path(relative: str, mode: str) -> bool:
    if relative == ("aphelion.exe" if mode == "windows" else "aphelion"):
        return True
    if relative == "LICENSE":
        return True
    if re.fullmatch(r"examples/.+\.agne", relative):
        return True
    return mode == "windows" and re.fullmatch(r"[^/]+\.dll", relative, flags=re.IGNORECASE) is not None


def _validate_manifest_files(manifest: dict[str, Any], members: dict[str, bytes],
                             root: str, mode: str) -> dict[str, dict[str, Any]]:
    files = manifest["files"]
    payload: dict[str, dict[str, Any]] = {}
    folded: set[str] = set()
    for item in files:
        if not isinstance(item, dict):
            fail("manifest.json has an invalid file entry")
        if not all(key in item for key in ("path", "kind", "size", "sha256")):
            fail("manifest.json has an incomplete file entry")
        relative = item["path"]
        _safe_name(relative, label="manifest file")
        if not _allowed_payload_path(relative, mode):
            fail(f"manifest.json has an unexpected member: {relative!r}")
        key = relative.casefold() if mode == "windows" else relative
        if key in folded:
            fail(f"manifest.json has duplicate members: {relative}")
        folded.add(key)
        size = item["size"]
        digest = item["sha256"]
        if type(size) is not int or size < 0:
            fail(f"manifest.json has an invalid file size: {relative}")
        if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
            fail(f"manifest.json has an invalid file hash: {relative}")
        expected_kind = (
            "native executable" if relative == ("aphelion.exe" if mode == "windows" else "aphelion")
            else "license" if relative == "LICENSE"
            else "runtime dependency" if mode == "windows" and relative.casefold().endswith(".dll")
            else "example/module"
        )
        if item["kind"] != expected_kind:
            fail(f"manifest.json has wrong file kind metadata: {relative}")
        full_name = f"{root}/{relative}"
        if full_name not in members:
            fail(f"archive is missing manifest file member: {relative}")
        data = members[full_name]
        if len(data) != size:
            fail(f"manifest file size mismatch: {relative}")
        if _sha256(data).casefold() != digest.casefold():
            fail(f"manifest file hash mismatch: {relative}")
        payload[relative] = item

    executable = "aphelion.exe" if mode == "windows" else "aphelion"
    if executable not in payload:
        fail("manifest is missing the release executable")
    if "LICENSE" not in payload:
        fail("manifest is missing the required LICENSE")
    if _sha256(members[f"{root}/{executable}"]).casefold() != manifest["executable_sha256"].casefold():
        fail("executable_sha256 mismatch")
    return payload


def _validate_checksums(data: bytes, members: dict[str, bytes], root: str,
                        payload: dict[str, dict[str, Any]]) -> None:
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"SHA256SUMS is malformed: not ASCII ({error})")
    if "\r" in text:
        fail("SHA256SUMS is malformed: use LF lines")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if not lines or any(not line for line in lines):
        fail("SHA256SUMS is malformed: blank line")

    expected_relative = set(payload) | {"manifest.json"}
    actual_relative: set[str] = set()
    for line in lines:
        match = CHECKSUM_LINE.fullmatch(line)
        if match is None:
            fail(f"SHA256SUMS is malformed: {line!r}")
        digest, relative = match.groups()
        _safe_name(relative, label="SHA256SUMS entry")
        if relative in actual_relative:
            fail(f"SHA256SUMS contains a duplicate member: {relative}")
        if relative not in expected_relative:
            fail(f"SHA256SUMS lists an unexpected member: {relative}")
        actual_relative.add(relative)
        full_name = f"{root}/{relative}"
        if _sha256(members[full_name]).casefold() != digest.casefold():
            fail(f"SHA256SUMS hash mismatch: {relative}")
    if actual_relative != expected_relative:
        missing = sorted(expected_relative - actual_relative)
        extra = sorted(actual_relative - expected_relative)
        fail(f"SHA256SUMS does not cover exactly the archive payload (missing: {missing}; extra: {extra})")


def validate_archive(archive: Path | str, mode: str, version: str) -> None:
    """Validate *archive* for *mode* and *version* without extracting it."""
    if mode not in ("windows", "linux"):
        fail(f"unsupported archive mode: {mode!r}")
    if not VERSION_PATTERN.fullmatch(version):
        fail(f"invalid version: {version!r}")
    path = Path(archive).expanduser()
    root = f"{PRODUCT}-{version}-{mode}"
    members = _zip_members(path, root) if mode == "windows" else _tar_members(path, root)
    manifest_name = f"{root}/manifest.json"
    checksums_name = f"{root}/SHA256SUMS"
    if manifest_name not in members or checksums_name not in members:
        fail("archive is missing manifest.json or SHA256SUMS")
    manifest = _parse_manifest(members[manifest_name], mode, version)
    payload = _validate_manifest_files(manifest, members, root, mode)

    expected_names = {manifest_name, checksums_name}
    expected_names.update(f"{root}/{relative}" for relative in payload)
    if set(members) != expected_names:
        missing = sorted(expected_names - set(members))
        extra = sorted(set(members) - expected_names)
        fail(f"archive has unexpected or missing members (missing: {missing}; extra: {extra})")
    _validate_checksums(members[checksums_name], members, root, payload)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("windows", "linux"), help="archive format and release platform")
    parser.add_argument("--archive", required=True, type=Path, help="ZIP or tar.gz archive to validate")
    parser.add_argument("--version", required=True, help="release version, for example 0.1.0")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        validate_archive(args.archive, args.mode, args.version)
    except (ArchiveValidationError, OSError) as error:
        print(f"Release archive FAILED: {error}", file=sys.stderr)
        return 1
    print(f"Release archive valid: {args.archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
