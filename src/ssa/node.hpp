// ssa/node.hpp — SSA module node definitions (Scaffold)
//
// This file defines the data structures for the planned SSA module.
// The SSA module is the intermediate representation between the typed
// AST and LLVM IR. See docs/ssa-ir.md for the design rationale.
//
// v0.1 does NOT implement SSA construction. These types exist so the
// AST can be shaped to lower into them cleanly when the time comes.

#pragma once

#include "support/intern.hpp"
#include "support/source.hpp"

#include <cstdint>
#include <vector>

namespace tether::ssa {

// Forward declaration. The SSA type system mirrors LLVM's first-class
// types: ints, floats, pointers, arrays, structs, named types. It is
// defined when the SSA construction pass is implemented.
struct Type;
using TypePtr = const Type*;

using ValueId = uint32_t;
using BlockId = uint32_t;

constexpr ValueId kInvalidValue = 0xFFFFFFFFu;
constexpr BlockId kInvalidBlock = 0xFFFFFFFFu;

// Opcodes. Kept close to LLVM IR's instruction set so that lowering
// is a one-to-one translation.
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

const char* opcode_name(Opcode op);

// A single SSA instruction. Instructions are immutable once placed in
// a Block; the SSA construction pass creates them and never mutates
// them afterward.
struct Instruction {
    Opcode               opcode = Opcode::ConstInt;
    TypePtr              type = nullptr;       // forward-declared; see below
    ValueId              result = kInvalidValue;
    std::vector<ValueId> operands;
    SourceRange          loc;

    // Side data.
    uint64_t             int_data  = 0;
    StrId                str_data  = kInvalidStrId;
    BlockId              target    = kInvalidBlock;        // Br
    std::vector<BlockId> targets;                          // CondBr, Switch
    std::vector<ValueId> phi_inputs;                       // Phi (alternating value, block)
};

struct Block {
    BlockId                  id = kInvalidBlock;
    std::vector<Instruction> instructions;
    std::vector<BlockId>     predecessors;
    std::vector<BlockId>     successors;
};

struct Function {
    StrId                name = kInvalidStrId;
    std::vector<Block>   blocks;
    BlockId              entry = kInvalidBlock;
    std::vector<ValueId> params;
    std::vector<TypePtr> param_types;
    TypePtr              return_type = nullptr;
};

struct Global {
    StrId    name = kInvalidStrId;
    TypePtr  type = nullptr;
    ValueId  initializer = kInvalidValue;
    bool     is_const = false;
};

struct ExternDecl {
    StrId    name = kInvalidStrId;
    std::vector<TypePtr> param_types;
    TypePtr  return_type = nullptr;
    bool     is_variadic = false;
};

struct Module {
    std::vector<Function>   functions;
    std::vector<Global>     globals;
    std::vector<ExternDecl> externs;
    StrId                   module_name = kInvalidStrId;
};

} // namespace tether::ssa
