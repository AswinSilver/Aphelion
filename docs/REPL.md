# One-line, fresh-scope REPL

`aphelion repl` runs an actual compiler/bytecode/VM read-eval-print loop,
not a shell, AST interpreter, or rewritten source script. This is the bounded
first interactive version; it does **not** promise persistent state or multiline
editing.

- Each physical line is an independent program with a fresh semantic scope,
  bytecode, and VM. Declarations do not persist to the next input line. Use
  semicolons/braces to put related declarations/statements on one line.
- All existing expressions/statements work within that one line, except
  `summon`, which is file-only in this initial REPL. Bare top-level expression
  values are echoed by ordinary VM REVEAL instructions; `silent` results are
  not echoed again. File execution and expressions inside function/loop/block
  bodies keep their existing behavior.
- No source wrapping or text replacement is used for echo: an explicit compiler
  option defaults off and is enabled only for the REPL.
- `:help` describes this contract; `:quit` and `:q` end the session. EOF exits
  cleanly, evaluating a final nonempty line even without a final newline.
- Empty/comment lines are harmless. Incomplete syntax is diagnosed at that
  line, never implicitly buffered as a multiline program. The next line starts
  fresh after syntax, semantic, runtime, or assertion errors.
- Input lines are bounded to 64 KiB of source bytes. Overlong/raw-NUL lines are
  rejected and drained safely. I/O failures are diagnostics. All temporary AST,
  compiler, bytecode, runtime, and input storage is released between inputs.
- Prompts/banner appear only on an interactive terminal, not redirected input.
  `receive()` consumes data from the same stdin stream, so the next line may be
  its data rather than another REPL command.
- The final status is 0 if every evaluation succeeded, or 1 if any input/eval
  failed; invalid command-line arguments still return 2. A failure can therefore
  recover interactively without being hidden from a script's exit status.

## Try it

```text
aphelion repl
aphelion> 1 + 2
3
aphelion> forge n = 3; n * 2
6
aphelion> n
<repl>:1:1: undefined name 'n'
aphelion> :quit
```

The undefined-name line deliberately demonstrates fresh scope; this session
returns status 1 despite recovering. For a successful redirected sample:

```sh
./aphelion repl < examples/repl_lines.agne
```

Expected output is `3`, `6`, `6`, `1`, `2`, on separate lines, status 0.
Prompts/banner are absent in redirected input. Use `aphelion.exe` on Windows.
Actual platform/check results are recorded in `../DEVELOPMENT_STATUS.md`.
Persistent declarations, multiline input/history, and imports in REPL sessions
remain future extensions, not hidden promises.
