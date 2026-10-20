# Changelog

All notable changes to Aphelion releases will be documented here.

## [Unreleased]

- Added a single checked-in release identity in `include/aphelion/version.h`.
- Added source, binary, tag, and package version consistency checks to release
  validation.
- Added the MIT License at the repository root and require it in release
  archives, manifests, and checksum coverage.
- Added a manual, environment-protected public release workflow that requires
  an existing `vX.Y.Z` tag and never pushes tags.
- Documented the MIT licensing and archive inclusion requirements.

## [0.1.0]

- Initial Stage 1 compiler, VM, CLI, and review-only binary packaging baseline.
