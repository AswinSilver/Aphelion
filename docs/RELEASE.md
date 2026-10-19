# Stage 1 release packages

Stage 1 produces reviewable binary packages for **Windows x64 UCRT64** and
**Linux x64**. The version input defaults to the current CLI version, `0.1.0`.
The package workflow is deliberately manual: select **Actions > Stage 1 release
packages > Run workflow**, enter a version, and inspect the two resulting
Actions artifacts before distributing them.

## Package contents

The workflow builds and tests a native executable with the repository's strict
C11 flags, runs `tools/release_smoke.py`, checks the native file identity and
runtime imports, and then calls `tools/release_package.py` with the exact tested
executable. The manually supplied version must match the executable's embedded
`aphelion --version` value; changing the package label alone is rejected. The
archive names are:

```text
aphelion-VERSION-windows.zip
aphelion-VERSION-linux.tar.gz
```

Each archive has one top-level directory named after the archive and contains:

- `aphelion.exe` (Windows) or `aphelion` (Linux), the tested native CLI;
- the `.agne` examples, including `examples/lib/math.agne`, used by the module
  example;
- any non-system Windows runtime files discovered for that native executable,
  beside the executable;
- `manifest.json`, recording the version, platform, native identity,
  dependency inspector, executable hash, dependency information, and every
  payload file hash; and
- `SHA256SUMS`, containing a SHA-256 entry for every payload file and the
  manifest. The checksum file intentionally does not contain its own recursive
  checksum.

The workflow uploads the archive and its adjacent checksum sidecar,
`aphelion-VERSION-PLATFORM.(zip|tar.gz).sha256`, as Actions artifacts. The
sidecar contains the SHA-256 of the archive itself. System libraries are not
copied into a package; the manifest identifies them as system dependencies.

## Installation and PATH

Packages are archives, not installers. Extract one into a directory that can
remain in place; keep the archive's top-level directory intact so the bundled
examples and modules retain their relative layout.

### Windows x64 UCRT64

Extract `aphelion-0.1.0-windows.zip`, then add its directory to the user PATH.
For a PowerShell session, for example:

```powershell
$install = "$HOME\Apps\aphelion-0.1.0-windows"
$env:Path = "$install;$env:Path"
aphelion.exe --version
```

To persist the directory for future terminals, add it through **Environment
Variables** in Windows settings, or use `setx PATH` carefully without
discarding the existing PATH. Start a new terminal after changing the user
PATH. The executable is built for the UCRT64 x64 environment; Windows system
DLLs are supplied by the operating system, while any non-system toolchain DLL
needed by the executable is placed beside it and listed in `manifest.json`.

### Linux x64

The Linux package is built on Ubuntu 22.04 and checked against a **glibc 2.35
baseline**. Extract it and add the extracted directory to the current shell's
PATH:

```sh
mkdir -p "$HOME/opt"
tar -xzf aphelion-0.1.0-linux.tar.gz -C "$HOME/opt"
export PATH="$HOME/opt/aphelion-0.1.0-linux:$PATH"
aphelion --version
```

Put the `export PATH=...` line in the shell's profile to make it persistent.
The binary is intended for x86-64 Linux systems with glibc 2.35 or newer and
compatible system libraries. Older glibc systems are not promised by this
package. `libm`, the C runtime, the dynamic loader, and other system libraries
remain host dependencies rather than bundled files.

## Running Aphelion programs

Run source explicitly with the `run` command:

```sh
aphelion run examples/hello.agne
aphelion run examples/factorial.agne
aphelion check examples/hello.agne
```

PowerShell uses the same command with `aphelion.exe` if the executable is not
on PATH:

```powershell
.\aphelion.exe run examples\hello.agne
```

Source paths must end in `.agne`. `summon` module paths are root-relative to
the directory containing the entry source, not to the process working
directory or an environment search path. For example,
`examples/modules.agne` uses `summon "lib/math"` and therefore needs
`examples/lib/math.agne` beside it. Preserve that layout when copying a source
tree or running the packaged examples. A module is linked into the in-memory
program; its source is not fetched at runtime from a registry.

`compile` writes a portable bytecode file beside the source and `disasm`
validates and displays one:

```sh
aphelion compile examples/hello.agne
aphelion disasm examples/hello.apb
```

The public CLI does **not** execute `.apb` files directly. Use `aphelion run
file.agne` to compile and execute source, even when a previous compile created
an `.apb` file. A compiled program is self-contained after loading its source
modules; it does not reload those modules during VM execution.

## Dependencies and runtime limits

End users need the native platform runtime libraries described by the archive
manifest, but do not need Python, a Python package, a separate Aphelion VM, or
another language runtime to run `.agne` source. Python and pytest are used by
the development/test workflow only. The executable never sends source to a
shell or host interpreter.

The packaging check uses `objdump -p` for Windows and `ldd` for Linux. It fails
on an unresolved non-system dependency. Windows API/system DLLs and Linux
system libraries are expected to come from the target operating system.
Resolved non-system Windows runtime files are included beside the executable
when the packaging tool can safely identify them. The current Linux binary
uses only host glibc/loader/system libraries, so no Linux private-library
search path is needed; the packager rejects a future Linux binary with a
private shared-library dependency until a verified RPATH/launcher is
implemented. A package is not a guarantee that an unsupported operating
system, CPU architecture, or older Linux C library will run it.

The archive contains the release examples and modules, not a general-purpose
module installation directory. When using a custom source tree, keep every
`summon` target under the entry source's canonical root and retain its `.agne`
file name. Module, source-size, VM-stack, and CLI limits remain those described
in the language and reliability documentation.

## Checksums and auditability

Verify the archive before extraction. On Linux:

```sh
sha256sum -c aphelion-0.1.0-linux.tar.gz.sha256
```

On PowerShell, compare the printed hash with the sidecar:

```powershell
(Get-FileHash .\aphelion-0.1.0-windows.zip -Algorithm SHA256).Hash.ToLower()
Get-Content .\aphelion-0.1.0-windows.zip.sha256
```

After extraction, `SHA256SUMS` can be checked from the archive's top-level
directory with a SHA-256 tool. Review `manifest.json` for the exact native
identity, dependency inspector output, dependency classification, executable
hash, and file list before moving the executable to a production PATH.

The packaging tool writes sorted archive members, normalized archive metadata,
and stable manifest/checksum text. For a reproducibility review, record the
repository commit, the workflow run URL, the manually supplied version, the
runner/toolchain versions printed in the log, the archive sidecar, and the
manifest. Rebuilding the same source and toolchain should produce the same
file-level contents; compiler and runner changes are audit inputs and can
change the native executable or dependency set.

## What this workflow publishes

Stage 1 publishes only the two downloadable **GitHub Actions artifacts** for
human review: one Windows x64 UCRT64 archive and one Linux x64 archive. It does
not create a GitHub Release, push a tag, publish to a package registry, deploy
an installer, or upload binaries to an external distribution service. A
successful workflow run is therefore a reviewed build result, not a public
release announcement.

## Workflow verification jobs

After each package is created, a separate Windows and Linux job downloads the
Actions artifact, verifies its archive checksum, extracts it, and runs the
version check, `examples/hello.agne`, and `examples/modules.agne` with a
sanitized PATH. These jobs deliberately do not install the UCRT64 toolchain,
Python, pytest, a C compiler, or the repository source. The hosted runner still
provides the operating system's normal system libraries; this is not a VM or a
claim for unsupported older Windows/Linux installations.
