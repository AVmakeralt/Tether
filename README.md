# Tether

A memory-safe systems programming language designed for writing compilers.

> **All safe code is memory-safe. Memory corruption is only possible inside
> explicitly marked `unsafe` code or through incorrect FFI contracts.**

Tether is built around a single constraint:

> *If it doesn't survive a million-line codebase, it doesn't make the language.*

This repository is the reference compiler, `tetherc`, written in C++17.

---

## Status

**v0.2 — Full pipeline to LLVM IR.**

The compiler lexes, parses, resolves names, type-checks, borrow-checks,
and emits LLVM IR text (`.ll`). Multi-file projects are supported via
the module loader.

| Stage          | Status         |
|----------------|----------------|
| Lexer          | implemented    |
| Parser         | implemented    |
| AST            | implemented    |
| Name resolution| implemented    |
| Type checker   | implemented    |
| Borrow checker | implemented    |
| SSA            | scaffold       |
| LLVM IR emit   | implemented    |
| LLVM codegen   | use `clang`/`llc` |

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
