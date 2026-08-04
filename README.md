# Tether

A memory-safe systems programming language designed for writing compilers.

> **All safe code is memory-safe. Memory corruption is only possible inside
> explicitly marked `unsafe` code or through incorrect FFI contracts.**

Tether is built around a single constraint:

> *If it doesn't survive a million-line codebase, it doesn't make the language.*

This repository is the reference compiler, `tetherc`, written in C++17.

---

## Status

**v0.3 — Full SSA pipeline with own IR + LLVM lowering.**

The compiler lexes, parses, resolves names, type-checks, borrow-checks,
lowers to Tether's own SSA IR, optimizes the SSA, and lowers to LLVM IR
text (`.ll`).

```
source → lexer → parser → AST → resolver → type checker →
         borrow checker → SSA builder → SSA optimizer → LLVM IR
```

| Stage          | Status         |
|----------------|----------------|
| Lexer          | implemented    |
| Parser         | implemented    |
| AST            | implemented    |
| Name resolution| implemented    |
| Type checker   | implemented    |
| Borrow checker | implemented    |
| SSA builder    | implemented    |
| SSA optimizer  | implemented    |
| SSA → LLVM IR  | implemented    |
| Partial eval   | implemented    |
| Incremental    | implemented    |

### Why not AST → LLVM IR directly?

LLVM cannot:
- verify ownership / borrows (no concept of move)
- track allocation domains (no concept of arena)
- enforce region / lifetime invariants (no concept of region)
- monomorphize generics (no concept of trait bounds)
- resolve trait dispatch (no concept of Tether traits)
- run user-defined `rewrite` rules
- do partial evaluation of arbitrary comptime functions
- compile pattern matches with structural patterns + guards
- elide bounds checks via range analysis on Tether semantics
- enforce the "no hidden allocations" invariant
- track unsafe boundaries
- do per-function incremental compilation

All of that happens on Tether's SSA module. LLVM only sees the final,
optimized SSA lowered to its own IR.

---

## Design Principles

1. **Memory safe by default** — no UB, no raw pointers, no null, no
   use-after-free, no data races in safe code.
2. **Explicit over clever** — every allocation is visible, ownership is
   visible, mutability is visible.
3. **Zero-cost abstractions** — everything compiles down to LLVM IR
   without runtime penalties.
4. **Stable semantics** — the language spec never depends on LLVM
   optimizations.

See [`docs/design.md`](docs/design.md) for the full design document.

---

## Keywords (34)

```
module  import  export

fn  struct  enum  union  trait  impl  type  alias

let  mut  const  static

if  else  match  while  for  loop  break  continue  return  defer

alloc  move  borrow  unsafe

extern  ffi  comptime

spawn  await
```

`self` is a contextual keyword. `true`/`false` are boolean literals.

---

## Build

```bash
cd Tether
make
```

The compiler binary is `bin/tetherc`.

### Run

```bash
# Emit LLVM IR (default)
./bin/tetherc path/to/program.tether

# Emit AST
./bin/tetherc --emit-ast path/to/program.tether

# Type check only
./bin/tetherc --check path/to/program.tether

# Write .ll to a file
./bin/tetherc -o program.ll path/to/program.tether
```

### Compile to an executable

```bash
./bin/tetherc -o program.ll program.tether
clang program.ll -o program
./program
```

### Multi-file projects

```bash
# Project layout:
#   myproj/
#     lib/
#       utils.tether
#     src/
#       main.tether    # import lib::utils

./bin/tetherc --stdlib myproj myproj/src/main.tether
```

The `--stdlib` flag sets the root directory for module resolution.
`import lib::utils` looks for `lib/utils.tether` relative to that root.

---

## Example

```tether
module test

fn add(a: i32, b: i32) -> i32 {
    return a + b
}

fn main() -> i32 {
    let x = add(3, 4)
    return x
}
```

Emits:

```llvm
define i64 @_tether_add(i64 %arg0, i64 %arg1) {
entry:
  %r1 = add i64 %arg0, %arg1
  ret i64 %r1
}

define i64 @_tether_main() {
entry:
  %r1 = add i64 3, 0
  %r2 = add i64 4, 0
  %r3 = call i64 @_tether_add(i64 %r1, i64 %r2)
  ret i64 %r3
}
```

---

## Repository Layout

```
Tether/
├── docs/              design documents
├── src/
│   ├── lexer/         tokenizer + token definitions
│   ├── parser/        Pratt parser + recursive descent
│   ├── ast/           immutable, arena-allocated AST nodes
│   ├── types/         type system + interning
│   ├── resolve/       name resolution + symbol tables
│   ├── check/         type checker
│   ├── borrow/        borrow checker
│   ├── llvm/          LLVM IR text emitter
│   ├── module/        multi-file module loader
│   ├── diagnostics/   source-located error reporting
│   ├── support/       Arena, InternTable, source buffers
│   ├── ssa/           planned SSA module (scaffold)
│   └── main.cpp       tetherc driver
├── stdlib/            core types (Option, Result)
├── tests/             unit tests + .tether fixtures
└── Makefile
```

---

## License

MIT. See `LICENSE`.
