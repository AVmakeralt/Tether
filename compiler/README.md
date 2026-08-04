# Tether Compiler Toolkit

A separate project from `std`. The language ships with a standard
library; the compiler ecosystem ships with this toolkit.

Think of it like LLVM itself: the toolkit provides the infrastructure
for writing compilers, but it is NOT part of the language's standard
library. It can evolve independently without freezing `std`.

## What belongs here (NOT in std)

```
ir/          intermediate representations
cfg/         control flow graph construction + analysis
ssa/         SSA construction, phi placement, dominance
backend/     target-specific lowering
optimizer/   optimization passes
codegen/     instruction selection + register allocation
parser/      parser infrastructure (combinators, Pratt, LL/LR)
lexer/       lexer infrastructure (DFA, regex-based)
```

## What does NOT belong here

```
Register allocator  → it's a policy, not a library
LLVM wrapper        → use LLVM directly
JIT                 → separate concern
```

## Relationship to std

The toolkit USES `std` but `std` does not depend on the toolkit.
A compiler written in Tether imports both:

```tether
import std::collections::densemap
import std::graph::algorithms
import compiler::ir::module
import compiler::ssa::builder
```

## Status

Scaffold. The reference Tether compiler (`tetherc`) currently
implements its own IR and SSA inline; a future version will migrate
to using this toolkit.
