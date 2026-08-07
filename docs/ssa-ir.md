# Tether SSA IR and LLVM Lowering — Design and Plan

> **v0.9 (zero-overhead FFI):** the SSA → LLVM lowering no longer
> widens integers to i64. Function signatures use the declared types
> (i32 stays i32, i8 stays i8), calling conventions propagate from
> `extern "C"` / `extern "fastcall"` / etc. to LLVM's `define` /
> `declare` / `call`, extern functions are referenced by their bare
> symbol name (no `_tether_` mangling), and call sites look up the
> callee's signature and emit matching argument types. The result is
> that a Tether call to `extern fn printf` lowers to a direct
> `call i32 (i8*, ...) @printf(...)` with no wrapper — exactly the
> "FFI as a type/ABI lowering system" architecture.

> **Note:** v0.1 does not implement lowering. This document describes
> the planned design so the AST is shaped to lower into it cleanly.

## No tree-walking interpreter

Tether does not have a tree-walking interpreter. Programs are compiled,
not interpreted. The pipeline is:

```
AST  ──[name resolution]──>  resolved AST
       ──[type checking]──>   typed AST
       ──[borrow checking]──> checked AST
       ──[SSA construction]──> SSA module
       ──[LLVM lowering]──>    LLVM module
       ──[LLVM opt + codegen]──>  object file
```

## Why SSA, not Sea of Nodes

The original draft of this document proposed a Sea-of-Nodes style IR
in the lineage of Click (V8 TurboFan, GraalVM, HotSpot server). After
review, Tether uses **traditional SSA form** instead — the same shape
LLVM IR itself uses, with explicit basic blocks, terminator
instructions, and phi nodes.

Reasons:

1. **Direct LLVM lowering.** Tether lowers to LLVM IR *immediately*
   after SSA construction. There is no intermediate IR layer between
   the typed AST and LLVM IR. Adopting an SSA shape that mirrors
   LLVM IR's own structure makes the lowering pass essentially a
   one-to-one translation — no shape translation, no information
   loss, no "lost in scope" problems where AST semantics disappear
   into a graph and have to be reconstructed.

2. **No graph rewrites to maintain.** Sea-of-Nodes optimizers trade
   implementation simplicity for graph rewrites that must be correct
   by construction. LLVM already has a mature, well-tested optimizer.
   Duplicating it inside Tether would be a multi-year project with
   no payoff — LLVM's passes are not the bottleneck for compiler
   engineers using Tether.

3. **Lower implementation cost.** Basic blocks, terminators, and phi
   nodes are the mental model every compiler engineer already has.
   The SSA module is a thin, predictable structure.

## What is "lost in scope"

The phrase comes from the spec author's correction: when an
intermediate IR abstracts away AST structure (variable names, scoping
boundaries, ownership tokens, allocation domains), that information
has to be reconstructed at the LLVM layer — or, worse, it is lost and
the LLVM IR no longer reflects the source program's ownership
invariants.

Tether avoids this by lowering **directly** to LLVM IR. The SSA
module is a structuring device for borrow checking and ownership
validation; it is not an independent optimization layer. Once SSA is
validated, it is emitted as LLVM IR with the original variable names
preserved as LLVM `local` names and ownership invariants encoded as
LLVM attributes (e.g. `noalias`, `nonnull`, `dereferenceable`).

## SSA module structure

```cpp
namespace ssa {

using ValueId = uint32_t;
using BlockId = uint32_t;

enum class Opcode : uint16_t {
    // Constants
    ConstInt, ConstFloat, ConstStr, ConstBool, ConstNull,
    // Arithmetic
    Add, Sub, Mul, Div, Mod, Neg,
    // Comparison
    Eq, Ne, Lt, Gt, Le, Ge,
    // Bitwise
    And, Or, Xor, Not, Shl, Shr,
    // Memory
    Alloc, Load, Store, Borrow, Move, Drop,
    // Control flow (terminators)
    Br, CondBr, Switch, Ret, Unreachable,
    // SSA
    Phi,
    // Calls
    Call, TailCall,
    // References
    Ref, Deref, FieldAddr, IndexAddr,
    // Casts
    BitCast, ZExt, SExt, Trunc,
};

struct Instruction {
    Opcode      opcode;
    Type        type;
    ValueId     result;
    std::vector<ValueId> operands;
    SourceRange loc;
    // Side data: integer constant value, call target name, etc.
    uint64_t    int_data = 0;
    StrId       str_data = kInvalidStrId;
};

struct Block {
    BlockId id;
    std::vector<Instruction> instructions;
    // Terminator is the last instruction; the block is unreachable if
    // empty.
};

struct Function {
    StrId                name;
    std::vector<Block>   blocks;
    BlockId              entry;
    std::vector<ValueId> params;
    std::vector<Type>    param_types;
    Type                 return_type;
};

struct Module {
    std::vector<Function>        functions;
    std::vector<Global>          globals;
    std::vector<ExternDecl>      externs;
};

} // namespace ssa
```

### Allocation domains on SSA

The allocation-domain model from `docs/design.md` is enforced at SSA
construction time. Every `Alloc` instruction carries the `ArenaId` it
belongs to. `Borrow` produces a reference tagged with that arena; the
SSA validator checks that references do not escape their arena's
dominating lifetime.

When the SSA module is lowered to LLVM, each arena becomes an LLVM
`alloca` of a bump-allocator slab (or, for the `heap` allocator, a
call to the runtime's `malloc`-equivalent). The arena's `Drop`
becomes a single `call` to the slab's free function — no per-object
destructors.

## Ownership on SSA

- `move` lowers to a `Move` instruction that consumes its input's
  ownership token. A subsequent use of the moved value is rejected by
  the SSA validator — the value has no dominating definition.
- `borrow` lowers to a `Borrow` instruction producing a `ref` typed
  value. The reference is tagged with a region; the validator ensures
  the region outlives every use.
- `alloc arena` lowers to an `Alloc` instruction tied to the arena's
  lifetime token; the arena's `Drop` is the single free.

## Memory state

Memory is threaded explicitly through SSA as a value:

```
%mem1 = Store %addr, %val, %mem0
%val2 = Load %addr, %mem1
```

This makes aliasing explicit and lets the borrow checker reason about
which loads depend on which stores — without requiring LLVM's own
alias analysis to be sound for Tether's safety proofs.

## Optimization

Tether does **not** run its own optimization passes by default. The
LLVM optimizer (`opt`) is run on the emitted LLVM IR. This is
deliberate: the language spec says LLVM is a backend, not part of the
language definition — but Tether does not forbid *using* LLVM's
optimizer. The contract is:

> If a Tether program's semantics change because an LLVM pass
> reorders or eliminates code, that is a bug in Tether's lowering,
> not in LLVM.

In other words: the LLVM IR Tether emits must already be correct
under all standard LLVM transformations. Tether does not rely on any
LLVM pass to make a program *correct*, only to make it *fast*.

### `rewrite` rules

The `rewrite` language feature compiles down to **AST-level
rewrites**, not SSA-level passes. A `rewrite ConstantFold { ... }`
block generates an AST-to-AST transformation that is applied before
SSA construction. This keeps rewrite rules scoped to the AST, where
the programmer can reason about them, and avoids the complexity of
maintaining a separate graph-rewrite engine.

## What v0.9 has

The `src/ssa/` directory contains:

- `ssa/node.hpp` — `Opcode`, `Instruction`, `Block`, `Function`,
  `Module` definitions, including `CallConv` propagation and a
  `StructLayout` table for real struct field types.
- `ssa/builder.cpp` — full AST → SSA lowering: basic blocks, phi
  nodes, mem-token threading, ownership/region/arena tracking,
  struct/enum construction, monomorphization-aware generics.
- `ssa/optimizer.cpp` — CSE, DCE, constant folding, SCCP, CFG
  simplification.
- `ssa/emit_llvm.cpp` — SSA → LLVM IR text lowering with proper
  basic blocks, calling conventions, real integer widths, zero-
  overhead FFI calls, and real struct layouts.
- `ssa/mono.cpp` — generic monomorphization with type-parameter
  substitution in signatures.
- `ssa/partial_eval.cpp` — compile-time function evaluation for
  `comptime` blocks and constant propagation.
- `ssa/rewrite.cpp` — AST-level rewrite rules.
- `ssa/incremental.cpp` — per-function incremental compilation
  cache (scaffolding).

### v0.9 FFI lowering — what "zero-overhead" means here

A Tether call to an extern function lowers to a direct LLVM `call`
to the bare C symbol — no wrapper, no trampoline, no conversion.
Concretely:

```tether
extern "C" fn printf(fmt: *const u8, ...) -> i32

fn main() -> i32 {
    printf("hello")
    return 0
}
```

lowers to:

```llvm
declare i32 @printf(i8*, ...)

define i32 @_tether_main() {
  %r1 = getelementptr [6 x i8], [6 x i8]* @.str.0, i64 0, i64 0
  %r2 = call i32 @printf(i8* %r1)
  %r3 = add i64 0, 0
  %r4 = trunc i64 %r3 to i32
  ret i32 %r4
}
```

The properties that make this zero-overhead:

1. **Bare symbol name.** The `call` references `@printf`, not
   `@_tether_printf` and not a generated wrapper. The linker
   resolves it directly to the C library function.
2. **Real types.** The `declare` uses `i32` and `i8*`, not the
   v0.2–v0.8 i64 widening. The call's argument and return types
   match the declare exactly.
3. **Calling convention propagated.** `extern "C"` lowers to LLVM's
   default `ccc` (omitted in text IR); `extern "fastcall"` lowers
   to `fastcc`; `extern "stdcall"` lowers to `x86_stdcallcc`; etc.
   The convention appears on both the `declare` and the `call`.
4. **Variadic handled.** `extern fn printf(fmt: *const u8, ...)`
   lowers to `declare i32 @printf(i8*, ...)`. Extra call args are
   passed after the fixed `i8*` parameter, exactly as C expects.
5. **Implicit casts at the boundary.** A Tether integer literal
   defaults to `i64`, but if it flows into an `i32` parameter, the
   emitter inserts a `trunc` at the call site. Same for `sext` /
   `zext` when widening. The programmer never writes these casts.

The same architecture applies to user-defined Tether functions
calling each other — they use the `_tether_` mangled name and the
declared signature, with the same boundary cast logic.

## What v0.9 deliberately does not have

- A tree-walking interpreter. Tether programs are compiled.
- A direct AST → LLVM lowering. The SSA module is the required
  intermediate stage — without it, the borrow checker has no place
  to enforce ownership.
- An in-tree optimizer. LLVM handles optimization. (Tether's own
  SSA optimizer runs only the passes needed for correctness — CSE,
  DCE, constant folding — not performance.)
- A JIT. The pipeline emits `.ll` text, which `clang` or `llc`
  lowers to machine code. An in-process ORC JIT is a future option.
- C++ ABI support. `extern "C++"` is parsed but not yet wired to
  the Itanium or Microsoft C++ ABI — only the calling convention
  is propagated. C++ name mangling, vtables, and exception
  unwinding are out of scope for v0.9.
- Per-target platform parameterization. C ABI aliases like `int`,
  `c_long`, `usize` are hardcoded to LP64 (Linux/macOS). Windows
  (LLP64) and 32-bit targets need a target-triple parameter.
