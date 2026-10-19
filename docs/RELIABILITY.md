# Reliability contract and reproducible checks

The original reliability stage hardened the compiler/VM before new language
features. This contract continues across the subsequent builtins, immutable
lists, traversal, modules, and bounded one-line REPL. The decisions preserve
the intended static types and make implementation boundaries explicit:

- `whole` operations and whole/whole comparisons are exact signed 64-bit
  operations. Overflow traps; it never changes the result type or wraps.
  Division truncates toward zero; remainder has the dividend's sign.
  `INT64_MIN / -1`, `INT64_MIN % -1`, and negating `INT64_MIN` trap.
- `-9223372036854775808` is a valid minimum whole literal; the same positive
  magnitude is not a whole value. Mixed numeric arithmetic/comparisons convert
  the whole operand to binary64, with its documented rounding above 2^53.
- Assigning/passing/returning a whole to a decimal inserts an actual widening
  conversion, not only a semantic annotation. Decimal-to-whole truncates first
  and accepts only the half-open interval [-2^63, 2^63), independently of the
  host's `long double` precision. Finite decimal arithmetic overflow traps.
- `text` is length-delimited byte data with a trailing convenience NUL. Escaped
  NUL bytes, input, concatenation, identity `to_text`, and `.apb` round-trips
  preserve its length. Source files themselves must not contain raw NUL bytes.
  UTF-8 identifiers remain unsupported; complete UTF-8 validation is deferred.
- `receive()` removes a line's LF and optional preceding CR, but preserves a
  final bare CR at EOF. Empty input and EOF return empty text.
- Malformed or excessively deep source is rejected with a diagnostic. Parser
  recursion and AST depth are bounded; valid programs cannot consume the host
  C stack without a language-level depth failure.
- Bytecode is an untrusted input. Section sizes, value tags, scalar ranges,
  function metadata, opcodes, operands, and jumps are checked before use. The VM
  keeps operands separate from frame-local slots and rejects underflow even in
  malformed in-memory bytecode. A VM instruction budget bounds runaway code.
- Top-level uses are checked in source order. An early function may reference a
  global whose type is known but whose initializer has not run; the VM reports
  this instead of substituting `hollow`. Conversion builtin names are reserved
  against redeclaration, and `silent` is not a binding/parameter value type.
- Allocation failures must unwind partial trees, compiler state, bytecode, and
  runtime values. Tests inject allocation failures rather than assuming the
  successful path proves cleanup correctness.
- Immutable lists retain exact homogeneous primitive elements and cannot form
  cycles. Traversal snapshots its iterable once and clears loop-private slots
  on exit. List/index bytecode validates tags, operands, frame floors and bounds.
- Modules parse independently under a canonical entry-file root, with bounded
  graphs/source sizes, cycle checks and once-only initialization. SOURCE markers
  preserve per-frame filenames in standalone bytecode. Path checks do not claim
  an OS sandbox against concurrent filesystem races.
- REPL input is one independent program per physical line, capped at 64 KiB.
  Raw NUL/overlong lines drain without partial execution; every line's objects
  are released. Echo is a default-off compiler option, not source rewriting.

The deterministic harnesses check finite corpora, not exhaustive correctness.
Sanitizers and fault injection supplement tests; none is a proof of memory
safety. See `DEVELOPMENT_STATUS.md` for actual commands, results, and gaps.
