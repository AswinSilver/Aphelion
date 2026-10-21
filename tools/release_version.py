#!/usr/bin/env python3
"""Read and validate Aphelion's checked-in release identity."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


PRODUCT = "aphelion"
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
VERSION_DEFINE = re.compile(r"^\s*#define\s+APHELION_VERSION\s+\"([^\"]+)\"\s*$", re.MULTILINE)
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "include" / "aphelion" / "version.h"


class VersionFailure(Exception):
    """A release identity precondition was not satisfied."""


def validate_version(version: str) -> str:
    if not VERSION_PATTERN.fullmatch(version):
        raise VersionFailure(
            f"invalid version {version!r}; expected a dotted version such as 0.1.0"
        )
    return version


def read_version(source: Path = DEFAULT_SOURCE) -> str:
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as error:
        raise VersionFailure(f"cannot read version source {source}: {error}") from error
    matches = VERSION_DEFINE.findall(text)
    if len(matches) != 1:
        raise VersionFailure(
            f"version source {source} must contain exactly one APHELION_VERSION definition"
        )
    return validate_version(matches[0])


def require_source_version(version: str, *, source: Path = DEFAULT_SOURCE) -> str:
    expected = read_version(source)
    supplied = validate_version(version)
    if supplied != expected:
        raise VersionFailure(
            f"version mismatch: requested {supplied}, checked-in source is {expected}"
        )
    return expected


def require_binary_version(
    version: str, executable: Path, *, source: Path = DEFAULT_SOURCE,
) -> str:
    expected = require_source_version(version, source=source)
    executable = executable.expanduser().resolve()
    if not executable.is_file():
        raise VersionFailure(f"executable is not a file: {executable}")
    try:
        result = subprocess.run(
            [str(executable), "--version"], capture_output=True,
            text=True, encoding="utf-8", errors="replace", check=False, timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VersionFailure(f"could not read binary identity from {executable}: {error}") from error
    expected_output = f"{PRODUCT} {expected}\n"
    if result.returncode != 0 or result.stdout != expected_output or result.stderr:
        raise VersionFailure(
            f"binary/source version mismatch: expected {expected_output!r}; "
            f"got status={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}"
        )
    return expected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="version to compare with the source")
    parser.add_argument("--executable", type=Path, help="also compare an executable's --version")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--print", action="store_true", dest="print_version")
    args = parser.parse_args(argv)
    try:
        source = args.source.resolve()
        version = read_version(source)
        if args.version is not None:
            if args.executable is not None:
                require_binary_version(args.version, args.executable, source=source)
            else:
                require_source_version(args.version, source=source)
        elif args.executable is not None:
            require_binary_version(version, args.executable, source=source)
    except VersionFailure as error:
        print(f"Release identity FAILED: {error}", file=sys.stderr)
        return 1
    if args.print_version or args.version is None:
        print(version)
    else:
        print(f"Release identity passed: {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
