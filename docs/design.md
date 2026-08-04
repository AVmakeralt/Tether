# Tether Design Document

## Motivation

Tether is a systems programming language aimed at people who write
compilers, static analyzers, and similar heavy symbolic tools. The goal
is not to be the 27th language claiming to replace C++. The goal is to
optimize the work of compiler engineers first, and to provide a
realistic memory-safety story that does not collapse under a
million-line codebase.

The defining constraint is:

> **If it doesn't survive a million-line codebase, it doesn't make the
> language.**

Every feature, every keyword, every type-system rule is evaluated
against this constraint.

---

## Core Principles

### 1. Memory safe by default

- No undefined behavior in safe code.
- No raw pointer arithmetic.
- No null references.
- No use-after-free.
- No data races.

The promise is stated precisely as:

> All safe code is memory-safe. Memory corruption is only possible
> inside explicitly marked `unsafe` code or through incorrect FFI
> contracts.

This is the strongest guarantee a systems language can realistically
provide while still interoperating with C and C++. It scales because the
amount of code requiring manual auditing stays small instead of
infecting the whole codebase.

### 2. Explicit over clever

- Every allocation is visible.
- Ownership is visible.
- Mutability is visible.

Hidden allocations are forbidden. A method call `foo.bar()` must not
secretly heap-allocate. If memory is needed, the programmer writes
`alloc heap Foo()` or `alloc arena Node()` and the source of the
allocation is on the page.

### 3. Zero-cost abstractions

- Everything compiles down to LLVM IR without runtime penalties.
- No hidden allocations.
- No hidden virtual dispatch.

### 4. Stable semantics

- The language spec never depends on LLVM optimizations.
- LLVM is a backend, not part of the language definition.

If the spec relies on LLVM to make code fast, then LLVM becomes part of
the language and the language inherits LLVM's bugs.

---

## Ownership

Every value has exactly one owner.

```tether
let ast = Parser.parse()

move module = ast
```

After `move`, `ast` is invalid. No double frees. No use-after-free.

`move` is explicit. Borrowing is explicit too:

```tether
borrow x
borrow mut x
```

instead of Rust's `&` and `&mut`. This makes the parser simpler and the
code more readable at the call site.

### Borrowing rules

- Many immutable borrows of the same value: allowed.
- One mutable borrow: allowed.
- Both at once: forbidden.

These rules are simple enough to prove correct, which is the only
reason they are in the language.

---

## Memory

Memory always comes from an allocator. There is no hidden global
allocator.

```tether
alloc arena = Arena()

let node = alloc arena AstNode(...)
```

or

```tether
alloc heap Foo()
```

Every allocation states where it came from.

### Allocation domains

Instead of tracking every individual object's lifetime, Tether tracks
**allocation domains**. Every node created via `alloc arena` belongs to
that arena. A reference carries an `ArenaID`, and the compiler verifies
`reference.arena == current_arena` rather than tracking per-object
lifetimes.

For compiler workloads this is a large simplification, because most
ASTs, HIRs, MIRs, and CFGs already live in arenas. The language models
the ownership pattern people actually use rather than forcing every node
through fine-grained ownership.

### Arena death

When an arena dies, every node in it dies. No recursive destructors. No
million-object free loops. The arena is freed in one shot.

---

## References

Instead of raw pointers:

```tether
ref Foo

mut ref Foo
```

References are guaranteed:

- non-null
- initialized
- lifetime-checked

If a value can be absent, use `Option<ref Foo>`. Every possible null is
explicit.

Raw pointers only exist inside `unsafe` FFI blocks.

### Region inference

Tether avoids Rust's lifetime syntax. The compiler infers regions
internally — users rarely see them. If inference fails, the programmer
can write one explicitly:

```tether
region parser

fn foo(node: ref(parser) Node)
```

95% of code never mentions regions.

---

## No pointer arithmetic

Safe code cannot write `ptr + 1`. Ever. Use slices instead:

```tether
Slice<T>
```

Bounds-checked. LLVM removes checks where it proves them unnecessary.

---

## Unsafe

```tether
unsafe {
    ffi_call()
}
```

`unsafe` is the only place where the following are legal:

- raw pointers
- pointer arithmetic
- manual allocation
- transmute
- inline assembly

The unsafe boundary is obvious during code review.

---

## Keywords

Tether has **34 keywords**. Every keyword earns its place.

```
module  import  export

fn  struct  enum  union  trait  impl  type  alias

let  mut  const  static

if  else  match  while  for  loop  break  continue  return  defer

alloc  move  borrow  unsafe

extern  ffi  comptime

spawn  await
```

`self` is a **contextual keyword** — only meaningful inside `impl`
blocks. Outside of methods it is just another identifier. Contextual
keywords are one of the few modern language ideas that genuinely reduce
complexity instead of creating it.

`true` and `false` are boolean literals, not keywords.

### Banned keywords

These have caused enough collective suffering and are explicitly **not**
in the language:

```
goto  friend  volatile  register  mutable  inline  virtual
delete  new  using  typedef
```

Either obsolete, redundant, or replaceable with better mechanisms.

### Reserved for future evolution

```
async  yield  macro  operator  reflect
```

Reserved even if version 1 does not implement them, so existing code
does not break when they are added.

---

## Error Handling

No exceptions. No `try`, `catch`, `throw`.

```tether
fn parse() -> Result<AST, Error>

let ast = parse()?
```

Simple. Predictable. The `?` operator propagates errors; everything
else is explicit.

---

## Pattern Matching

```tether
match expr {
    Binary(Add, Int(a), Int(b)) => Int(a + b),
    Binary(Mul, x, Int(1))      => x,
    _                            => expr,
}
```

Pattern matching is the primary branching mechanism for structured data.

---

## Generics

Compile-time only. Monomorphized. No runtime generic system.

```tether
fn max<T: Ord>(a: T, b: T) -> T
```

---

## Traits

Small. No inheritance. No diamond problems.

```tether
trait Hash {
    fn hash(self) -> u64
}
```

---

## Modules

```tether
lexer/
parser/
hir/
mir/
backend/
```

```tether
import parser.ast
```

Simple hierarchy. Visibility is controlled by `export` — everything else
is private to the module. One keyword beats two.

---

## FFI

### C

```tether
ffi "stdio.h"

extern fn printf(fmt: *const c_char, ...) -> int
```

Calls become direct.

### C++

Much harder. Tether does not pretend to understand arbitrary C++.
Instead it uses generated bindings:

```tether
ffi cpp "vector"

extern class std::vector<T>
```

A binding generator using Clang's AST exposes supported C++ APIs while
respecting ABI details. This keeps the language sane instead of trying
to parse every template metaprogramming trick ever committed to Git.

---

## Built-in Compiler Collections

Because Tether is for compiler engineers, the standard ecosystem
includes the data structures that compilers actually need:

```
Arena<T>
InternTable<T>
DenseMap<K, V>
SmallVec<T, N>
BitSet
SparseBitSet
Graph<T>
CFG
SSA
```

These are not third-party libraries. They are part of the language's
standard ecosystem.

---

## Immutable ASTs

Trees are immutable.

```tether
let expr = Binary(Add, lhs, rhs)
```

A rewrite creates a new node with structural sharing — children that
did not change are reused by reference. LLVM and Rust have both taught
us that immutable compiler data structures make large optimization
pipelines dramatically easier to reason about.

---

## Concurrency

Fearless by default.

```tether
spawn {
    optimize(module)
}

await
```

No shared mutable state unless synchronization is explicit.

---

## Rewrite Rules

The single non-obvious feature: **rewrite rules are first-class**.

Instead of writing visitors manually:

```tether
rewrite ConstantFold {
    add(Int(a), Int(b)) => Int(a + b),
    mul(x, Int(1))      => x,
    mul(x, Int(0))      => Int(0),
}
```

The compiler generates the traversal, the matching, and the
replacement. This removes thousands of lines of repetitive optimizer
code and fits the language's purpose: optimize *the compiler engineer's
work* first.

---

## What Tether Is Not

- Not a garbage-collected language.
- Not a Rust clone. Ownership is explicit (`move`), borrowing is
  explicit (`borrow`), and the syntax is intentionally different.
- Not a C++ replacement in the general-purpose sense. It is a language
  for writing compilers and similar tools.
- Not "absolutely memory safe" in the marketing sense. It is memory
  safe in safe code, and `unsafe` exists for the cases where the
  programmer needs to step outside that boundary.
