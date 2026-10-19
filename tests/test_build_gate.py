"""Regressions for selecting, building, and checking the release test binary."""

import os
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

import conftest as infrastructure


@pytest.mark.parametrize("arguments", [
    [r"C:\Toolchains\GCC\bin\gcc.exe", "-O2"],
    [r"C:\Program Files\GCC\bin\gcc.exe", r"-IC:\header files\include"],
    ["ccache", r"C:\Program Files\GCC\bin\gcc.exe", "-Werror"],
    [r"C:\Program Files\GCC\bin\gcc.exe", '-DNAME="quoted value"'],
    ["gcc", "C:\\include files\\", "", r"C:\include\"quoted\""],
])
def test_windows_compiler_arguments_preserve_paths_and_quotes(monkeypatch, arguments):
    monkeypatch.setattr(infrastructure, "WINDOWS", True)
    assert infrastructure.split_arguments(subprocess.list2cmdline(arguments)) == arguments


def test_posix_compiler_arguments_keep_shell_quoting(monkeypatch):
    monkeypatch.setattr(infrastructure, "WINDOWS", False)
    arguments = ["ccache", "/compiler tools/gcc", "-DNAME=two words", "-O2"]
    assert infrastructure.split_arguments(shlex.join(arguments)) == arguments


def test_cc_accepts_an_unquoted_existing_compiler_path_with_spaces(monkeypatch, tmp_path):
    compiler = tmp_path / "compiler tools" / "gcc.exe"
    compiler.parent.mkdir()
    compiler.touch()
    monkeypatch.setenv("CC", str(compiler))
    assert infrastructure.compiler_command() == [str(compiler)]


def test_cc_and_extra_flags_use_windows_argument_rules(monkeypatch):
    monkeypatch.setattr(infrastructure, "WINDOWS", True)
    compiler = [r"C:\Program Files\GCC\bin\gcc.exe", "-O2"]
    flags = [r"-IC:\header files\include", "-fsanitize=undefined"]
    monkeypatch.setenv("CC", subprocess.list2cmdline(compiler))
    monkeypatch.setenv("APHELION_TEST_CFLAGS", subprocess.list2cmdline(flags))
    assert infrastructure.compiler_command() == compiler
    assert infrastructure.test_cflags() == flags


def test_empty_cc_is_a_configuration_error(monkeypatch):
    monkeypatch.setenv("CC", "")
    with pytest.raises(AssertionError, match="CC must specify"):
        infrastructure.compiler_command()


@pytest.mark.parametrize("windows", [False, True])
def test_build_outputs_and_math_link_flags_are_native(monkeypatch, windows):
    monkeypatch.setattr(infrastructure, "WINDOWS", windows)
    assert infrastructure.native_executable_name("aphelion") == (
        "aphelion.exe" if windows else "aphelion"
    )
    assert infrastructure.math_libraries() == ([] if windows else ["-lm"])


def test_prebuilt_binary_is_selected_exactly_without_rebuilding(monkeypatch, tmp_path):
    selected = tmp_path / "release candidate.exe"
    selected.touch()
    monkeypatch.setenv("APHELION_TEST_EXECUTABLE", str(selected))
    # No build factory: using it would demonstrate an unintended fallback build.
    assert infrastructure.aphelion_executable.__wrapped__(None) == selected.resolve()


@pytest.mark.parametrize("selection", ["", "missing.exe"])
def test_invalid_prebuilt_binary_never_falls_back(monkeypatch, tmp_path, selection):
    value = str(tmp_path / selection) if selection else ""
    monkeypatch.setenv("APHELION_TEST_EXECUTABLE", value)
    with pytest.raises(AssertionError, match="APHELION_TEST_EXECUTABLE"):
        infrastructure.aphelion_executable.__wrapped__(None)


def test_windows_missing_prebuilt_does_not_select_an_exe_sibling(monkeypatch, tmp_path):
    monkeypatch.setattr(infrastructure, "WINDOWS", True)
    selected = tmp_path / "aphelion"
    selected.with_suffix(".exe").touch()
    monkeypatch.setenv("APHELION_TEST_EXECUTABLE", str(selected))
    with pytest.raises(AssertionError, match="does not exist"):
        infrastructure.aphelion_executable.__wrapped__(None)


@pytest.mark.parametrize("windows", [False, True])
def test_existing_prebuilt_is_not_rewritten_based_on_platform(monkeypatch, tmp_path, windows):
    monkeypatch.setattr(infrastructure, "WINDOWS", windows)
    executable = tmp_path / "aphelion"
    executable.touch()
    monkeypatch.setenv("APHELION_TEST_EXECUTABLE", str(executable))
    assert infrastructure.aphelion_executable.__wrapped__(None) == executable.resolve()


@pytest.mark.parametrize("diagnostic", [
    "ERROR: AddressSanitizer: heap-use-after-free",
    "ERROR: LeakSanitizer: detected memory leaks",
    "UndefinedBehaviorSanitizer: undefined-behavior",
    r"C:\sources\vm.c:123:4: runtime error: signed integer overflow",
    "include/value.h:12: runtime error: null pointer",
])
@pytest.mark.parametrize("stream", ["stdout", "stderr"])
def test_expected_error_status_does_not_hide_sanitizers(diagnostic, stream):
    result = subprocess.CompletedProcess(["aphelion"], 1, stdout=b"", stderr=b"")
    setattr(result, stream, b"\xff" + diagnostic.encode("utf-8"))
    with pytest.raises(pytest.fail.Exception, match="sanitizer report in child process"):
        infrastructure.reject_sanitizer_output(result)


def test_language_runtime_errors_are_not_mistaken_for_ubsan():
    result = subprocess.CompletedProcess(
        ["aphelion"], 1, stdout="", stderr="program.agne:2:1: runtime error: division by zero"
    )
    infrastructure.reject_sanitizer_output(result)


@pytest.mark.parametrize("exception_type", [subprocess.CalledProcessError, subprocess.TimeoutExpired])
def test_exception_output_is_checked_before_propagating(exception_type):
    diagnostic = b"vm.c:3:4: runtime error: null pointer"
    if exception_type is subprocess.CalledProcessError:
        error = exception_type(1, ["aphelion"], stderr=diagnostic)
    else:
        error = exception_type(["aphelion"], 1, stderr=diagnostic)

    def failing_run(*args, **kwargs):
        raise error

    with pytest.raises(pytest.fail.Exception, match="sanitizer report in child process"):
        infrastructure.checked_subprocess_run(failing_run)(["aphelion"], check=True)


def test_text_subprocesses_use_utf8_and_binary_subprocesses_stay_binary():
    command = [sys.executable, "-c", "import sys; sys.stdout.buffer.write(bytes([0xc3, 0xa9]))"]
    text = subprocess.run(command, capture_output=True, text=True, timeout=5)
    binary = subprocess.run(command, capture_output=True, timeout=5)
    assert text.returncode == binary.returncode == 0
    assert text.stdout == "é"
    assert binary.stdout == b"\xc3\xa9"


def test_invalid_utf8_does_not_conceal_sanitizer_report():
    command = [
        sys.executable, "-c",
        "import sys; sys.stderr.buffer.write(b'\\xffAddressSanitizer: gate probe')",
    ]
    with pytest.raises(pytest.fail.Exception, match="sanitizer report in child process"):
        subprocess.run(command, capture_output=True, text=True, timeout=5)


def test_normal_compiler_warnings_and_exceptions_are_preserved():
    warning = "source.c:3: warning: ordinary compiler diagnostic"
    result = subprocess.CompletedProcess(["gcc"], 0, stdout="", stderr=warning)
    checked = infrastructure.checked_subprocess_run(lambda *args, **kwargs: result)
    assert checked(["gcc"]) is result
    assert result.stderr == warning

    error = subprocess.CalledProcessError(1, ["gcc"], stderr=warning)

    def failing_run(*args, **kwargs):
        raise error

    with pytest.raises(subprocess.CalledProcessError) as captured:
        infrastructure.checked_subprocess_run(failing_run)(["gcc"], check=True)
    assert captured.value is error


@pytest.mark.parametrize("phase", ["session-build", "expected-error"])
@pytest.mark.parametrize("stream", ["stdout", "stderr"])
def test_sanitizer_guard_covers_real_pytest_fixture_ordering(tmp_path, phase, stream):
    # An isolated pytest session verifies fixture ordering rather than calling
    # the wrapper directly. Its intentionally failing output must be inspected
    # via Popen, so the outer guard does not consume the expected failure.
    shared = Path(infrastructure.__file__).read_text(encoding="utf-8")
    (tmp_path / "conftest.py").write_text(shared, encoding="utf-8")
    compiler = tmp_path / "fake compiler.py"
    compiler.write_text(
        f"import sys\nsys.{stream}.write('ERROR: AddressSanitizer: gate probe\\n')\n"
        f"sys.exit({0 if phase == 'session-build' else 1})\n",
        encoding="utf-8",
    )
    if phase == "session-build":
        test = "def test_gate(aphelion_executable):\n    assert aphelion_executable\n"
    else:
        test = (
            "import subprocess, sys\n"
            "def test_gate():\n"
            f"    result = subprocess.run([sys.executable, {str(compiler)!r}], "
            "capture_output=True, text=True)\n"
            "    assert result.returncode == 1\n"
        )
    (tmp_path / "test_gate.py").write_text(test, encoding="utf-8")
    environment = os.environ.copy()
    environment.pop("APHELION_TEST_EXECUTABLE", None)
    environment.pop("APHELION_TEST_CFLAGS", None)
    environment["CC"] = (
        subprocess.list2cmdline([sys.executable, str(compiler)])
        if os.name == "nt" else shlex.join([sys.executable, str(compiler)])
    )
    environment["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    with subprocess.Popen(
        [sys.executable, "-m", "pytest", "-q", "--confcutdir", str(tmp_path), "test_gate.py"],
        cwd=tmp_path, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8",
    ) as process:
        stdout, stderr = process.communicate(timeout=30)
    assert process.returncode == 1, stdout + stderr
    assert "sanitizer report in child process" in stdout + stderr
    assert ("1 error" if phase == "session-build" else "1 failed") in stdout
