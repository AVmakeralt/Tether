// ssa/node.hpp — Tether SSA module: full instruction set + memory state
//
// The SSA module is Tether's own IR. It sits between the typed AST and
// LLVM IR. LLVM only sees the final, optimized SSA module lowered to
// its own IR — Tether does all ownership, region, arena, generic, and
// rewrite-rule work here, on SSA.
//
// Why not lower directly to LLVM IR?
//
//   LLVM cannot:
//     - verify ownership / borrows (no concept of move)
//     - track allocation domains (no concept of arena)
//     - enforce region / lifetime invariants (no concept of region)
//     - monomorphize generics (no concept of trait bounds)
//     - resolve trait dispatch (no concept of Tether traits)
//     - run user-defined `rewrite` rules
//     - do partial evaluation of arbitrary comptime functions
//     - compile pattern matches with structural patterns + guards
//     - elide bounds checks via range analysis on Tether semantics
//     - enforce the "no hidden allocations" invariant
//     - track unsafe boundaries
//     - do per-function incremental compilation
//
//   All of that happens on SSA. LLVM handles the final machine-code
//   generation: register allocation, instruction selection, scheduling.
//
// SSA shape:
//   - Traditional SSA with basic blocks and phi nodes (NOT sea-of-nodes).
//   - Memory is an explicit value: Store produces a new mem token.
//     This makes aliasing visible to Tether's own passes.
//   - Every instruction carries a SourceRange for diagnostics.
//   - Every allocation carries an ArenaId; the validator checks that
//     references do not escape their arena's lifetime.

#pragma once

#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tether::ssa {

using type::TypePtr;

using ValueId = uint32_t;
using BlockId  = uint32_t;
using ArenaId  = uint32_t;
using RegionId = uint32_t;

constexpr ValueId kInvalidValue = 0xFFFFFFFFu;
constexpr BlockId kInvalidBlock = 0xFFFFFFFFu;
constexpr ArenaId kNoArena      = 0;        // heap or static
constexpr RegionId kStaticRegion = 0;

// ---- Opcodes -----------------------------------------------------------
//
// Kept close to LLVM IR's instruction set so that the final lowering
// is a one-to-one translation. Memory and ownership opcodes are
// Tether-specific and have no LLVM equivalent — they're resolved
// during SSA validation and optimization, never emitted to LLVM.

enum class Opcode : uint16_t {
    // ---- Constants ----
    ConstInt,        // int_data = value
    ConstFloat,      // float_data = value
    ConstStr,        // str_data = interned id
    ConstBool,       // int_data = 0 or 1
    ConstNull,       // null pointer (unsafe only)
    ConstArray,      // operands = element values; type = array type

    // ---- Arithmetic ----
    Add, Sub, Mul, Div, Mod, Neg,
    // ---- Bitwise ----
    And, Or, Xor, Not, Shl, Shr,
    // ---- Comparison ----
    Eq, Ne, Lt, Gt, Le, Ge,

    // ---- Memory (Tether-specific) ----
    //
    // Memory is threaded as an explicit value. Each memory-producing
    // instruction takes the current mem token as an operand and
    // produces a new one. This makes aliasing visible: a Load is
    // dependent on the Store that could have written to its address,
    // not on every prior Store.
    Alloc,           // allocate in an arena; produces (ptr, new_mem)
    Load,            // load from address; produces (value, new_mem)
    Store,           // store value to address; produces new_mem
    Borrow,          // create a ref (carries region tag)
    Move,            // consume ownership of a value
    Drop,            // release a value (arena drop or heap free)
    MemPhi,          // phi for mem tokens at block joins

    // ---- Control flow (terminators) ----
    Br,              // unconditional branch
    CondBr,          // conditional branch
    Switch,          // multi-way branch
    Ret,             // return
    Unreachable,     // trap

    // ---- SSA ----
    Phi,             // phi node

    // ---- Calls ----
    Call,            // function call
    TailCall,        // tail call

    // ---- References ----
    Ref,             // create a reference to a local (alloca-like)
    Deref,           // dereference a ref (produces value)
    FieldAddr,       // compute field address (struct)
    IndexAddr,       // compute element address (array/slice)

    // ---- Casts ----
    BitCast,         // reinterpret bits
    ZExt,            // zero extend
    SExt,            // sign extend
    Trunc,           // truncate

    // ---- Struct / Enum ----
    StructConstruct, // build a struct from field values
    StructField,     // extract a field from a struct (by index)
    EnumConstruct,   // build an enum value (tag + payload)
    EnumGetTag,      // extract the tag from an enum
    EnumGetPayload,  // extract the payload from an enum

    // ---- Bounds checking ----
    BoundsCheck,     // abort if index >= len; produces new_mem
    // ---- Unsafe marker ----
    Unsafe,          // marker that the following instructions are
                     // inside an unsafe block (no LLVM equivalent)
};

const char* opcode_name(Opcode op);

// ---- Side data for instructions ----
//
// Instructions carry their opcode-specific data in a single union-like
// struct. This keeps Instruction small and cache-friendly.

struct Instruction {
    Opcode      opcode = Opcode::ConstInt;
    TypePtr     type   = nullptr;          // result type
    ValueId     result = kInvalidValue;    // SSA result register

    // Data dependencies (inputs). For most instructions these are the
    // operands. For Phi, they alternate (value, block, value, block, ...).
    // For MemPhi, they're mem tokens from predecessor blocks (in order).
    std::vector<ValueId> operands;
    std::vector<BlockId> blocks;   // for Phi, CondBr, Switch

    // Control input: the block this instruction belongs to.
    BlockId     block  = kInvalidBlock;

    // Memory input: for Load/Store/Alloc/Borrow/Move/Drop, this is the
    // incoming mem token. The instruction produces a new mem token
    // stored in `result` (for mem-producing ops) or alongside `result`
    // (for Load, which produces both a value and a new mem).
    ValueId     mem_in = kInvalidValue;

    SourceRange loc;

    // ---- Side data ----
    uint64_t    int_data   = 0;            // ConstInt, ConstBool
    double      float_data = 0.0;          // ConstFloat
    StrId       str_data   = kInvalidStrId;// ConstStr, Call target name
    ArenaId     arena      = kNoArena;     // Alloc
    RegionId    region     = kStaticRegion;// Borrow, Ref
    bool        is_unsafe  = false;        // marker for unsafe block
    uint32_t    field_index = 0;           // StructField, EnumConstruct (variant idx)

    // ---- For Call ----
    // The callee function name (interned). Direct calls only —
    // indirect calls (function pointers, trait dispatch) use a
    // separate opcode in a future version.
    // (str_data holds the callee name.)

    // ---- For Load ----
    // Load produces TWO results: the loaded value (in `result`) and
    // the new mem token (in `mem_out`).
    ValueId     mem_out = kInvalidValue;

    bool is_terminator() const {
        return opcode == Opcode::Br || opcode == Opcode::CondBr ||
               opcode == Opcode::Switch || opcode == Opcode::Ret ||
               opcode == Opcode::Unreachable || opcode == Opcode::TailCall;
    }

    bool is_mem_op() const {
        return opcode == Opcode::Alloc || opcode == Opcode::Load ||
               opcode == Opcode::Store || opcode == Opcode::Borrow ||
               opcode == Opcode::Move || opcode == Opcode::Drop ||
               opcode == Opcode::BoundsCheck;
    }
};

// ---- Basic block ----

struct Block {
    BlockId                  id = kInvalidBlock;
    std::vector<Instruction> instructions;
    std::vector<BlockId>     predecessors;
    std::vector<BlockId>     successors;
    // The mem token at the end of this block (for MemPhi at successors).
    ValueId                  exit_mem = kInvalidValue;
};

// ---- Function ----

struct Function {
    StrId                name = kInvalidStrId;
    std::vector<Block>   blocks;
    BlockId              entry = kInvalidBlock;
    std::vector<ValueId> params;
    std::vector<TypePtr> param_types;
    TypePtr              return_type = nullptr;

    // The initial mem token for the function (entry block's mem_in).
    ValueId              entry_mem = kInvalidValue;

    // Next free ValueId / BlockId for this function.
    ValueId              next_value = 1;  // 0 reserved for "no value"
    BlockId              next_block = 0;
};

// ---- Module ----

struct Global {
    StrId    name = kInvalidStrId;
    TypePtr  type = nullptr;
    ValueId  initializer = kInvalidValue;  // SSA value in a const-eval function
    bool     is_const = false;
};

struct ExternDecl {
    StrId                name = kInvalidStrId;
    std::vector<TypePtr> param_types;
    TypePtr              return_type = nullptr;
    bool                 is_variadic = false;
};

struct Module {
    std::vector<Function>   functions;
    std::vector<Global>     globals;
    std::vector<ExternDecl> externs;
    StrId                   module_name = kInvalidStrId;
    // Arena table: ArenaId -> human-readable name (for diagnostics).
    std::vector<StrId>      arenas;
};

// ---- Rendering ----
//
// Render an SSA module as human-readable text. Used by --emit-ssa and
// by tests. The format is line-oriented, similar to LLVM IR but with
// Tether-specific memory/ownership annotations.

std::string render_function(const Function& fn, type::TypeContext& tc);
std::string render_module(const Module& mod, type::TypeContext& tc);

} // namespace tether::ssa
