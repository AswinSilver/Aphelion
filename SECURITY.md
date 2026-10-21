# Security policy

## Supported versions

Security fixes are currently considered for the latest checked-in release
identity in `include/aphelion/version.h`. This project is pre-public-release;
support commitments and a long-term version policy will be established with
the first public release. Source and binary distribution are licensed under
the MIT License in the repository root `LICENSE` file.

## Reporting a vulnerability

Please do not open a public issue for a suspected vulnerability. Contact the
maintainers privately through the repository's configured security advisory or
private maintainer channel, including:

- the affected commit or release tag;
- a minimal reproducer or input file, when safe to share;
- the platform, compiler, and command used; and
- the observed impact and any available mitigation.

Allow maintainers reasonable time to investigate and coordinate a fix before
public disclosure. Do not include credentials, private source, or personal
data in a report.

## Reporting a download or antivirus false positive

Do not disable security protections or ask users to whitelist an Aphelion
artifact. Report the exact archive or executable SHA-256, release/tag or
commit, download URL, operating system, browser/security-product version, full
warning text, and any vendor case or diagnostic identifier through the same
private maintainer channel. Keep the original file available for reproducible
analysis. A third-party scan result does not by itself establish that a
warning is false or identify which component triggered it.
