# Tether SSA IR and LLVM Lowering — Design and Plan

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

## What v0.1 has

The `src/ssa/` directory currently contains:

- `ssa/node.hpp` — `Opcode`, `Instruction`, `Block`, `Function`,
  `Module` definitions (scaffolding only).

The SSA construction pass itself is **not** implemented. It will be
added in a later milestone, once name resolution and type checking
exist.

## What v0.1 deliberately does not have

- A tree-walking interpreter. Tether programs are compiled.
- A direct AST → LLVM lowering. The SSA module is the required
  intermediate stage — without it, the borrow checker has no place
  to enforce ownership.
- A textual IR printer for the SSA module. Planned, but the module
  shape is still in flux.
- An in-tree optimizer. LLVM handles optimization.
