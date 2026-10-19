# First collection: immutable homogeneous lists

Design proposed before implementation and now implemented for this first
collection stage. Actual Windows/Linux/sanitizer results are recorded in
`../DEVELOPMENT_STATUS.md`; later collection features are not implied.

## Surface and types

- Literal: `[1, 2, 3]`; annotation: `[whole]`, `[decimal]`, `[signal]`, `[text]`,
  or `[glyph]`. These are instances of one collection type, not separate
  collection implementations.
- Nonempty literals infer one exact element type. Mixed whole/decimal lists
  are rejected initially; write explicit decimal elements/conversions instead.
  Nested lists, hollow/silent/function elements, records, and maps are deferred.
- An empty binding requires an annotation: `forge xs: [text] = []`. Empty
  literals can be passed/returned/assigned where a typed list is expected;
  `length([])` is 0 and `kind([])` is `"list"`. Indexing an untyped empty
  literal is a static error because no result type can be inferred.
- List element types are invariant. Assignments/arguments/returns must preserve
  the element type; no implicit whole-list widening.

## Operations and failure behavior

- `xs[index]` uses a whole, zero-based index; negative and out-of-range indexes
  fail at runtime. Non-whole indexes and non-list targets fail statically and
  are defensively checked by the VM. Empty typed-list indexing fails bounds.
- `length(list)` is its element count; text length continues to count bytes.
  `kind(list)` is `"list"`. Equality compares ordered elements and length,
  with type compatibility checked statically. Empty literals compare as empty
  sequences. Ordered comparison and arithmetic on lists are rejected.
- Lists are immutable: no indexed assignment, append, or removal. `shift xs`
  permits rebinding to another compatible list, not modifying shared storage.
- Output/conversion uses a bracketed, comma-separated representation; text
  elements are quoted and escape quotes, backslashes, controls, and NUL.

## Architecture and ownership

- Add list/index expression nodes to the AST, with bounded depth and cleanup
  of partially constructed element vectors. Bracket tokens already exist in
  the real lexer; no source rewriting is permitted.
- Represent primitive-list element types in the existing semantic type system.
  Keep declaration identity resolution and lexical shadowing unchanged.
- Append LIST/INDEX instructions. LIST stores count and element tag in its
  operands and builds from evaluated scalar values; INDEX consumes list/index
  and pushes a retained element. Validate operands before execution and respect
  frame floors/stack limits and the VM instruction budget.
- Runtime list storage is reference counted and owns retained element values.
  Copy/load/return retain, store/rebind release the previous reference, and
  pop/error/shutdown release every owned reference. With no nested/mutable
  collections, this first design cannot create reference cycles.
- Lists are reconstructed from scalar bytecode constants at runtime, not
  embedded as recursive `.apb` constants. Reject list/uninitialized constant
  tags; preserve the v1 layout and all existing opcode/tag numbers.

## Acceptance checks

Empty and populated lists; every supported element type; invariant type errors;
negative/large/non-whole indexes; mixed/nested-list rejection; immutable indexed
assignment rejection; nested scopes/shadowing; function arguments/returns;
rebindings/text lifetime/embedded NUL; serialization/read-write-execute;
allocation-failure cleanup; hostile bytecode operand/type/underflow checks;
strict builds, full regression, finite fuzzing, and sanitizers when available.
