// ssa/builder.hpp — AST → SSA lowering
//
// The builder walks the typed AST and emits SSA instructions. It:
//   - creates basic blocks and branches for control flow
//   - threads memory as an explicit value (mem tokens)
//   - tracks ownership (move semantics) and regions
//   - assigns ArenaIds to allocations
//   - inserts bounds checks for slice/array indexing
//   - marks unsafe regions
//
// The builder does NOT do optimization — it produces straightforward
// SSA that the optimizer pass will clean up.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "resolve/resolve.hpp"
#include "ssa/node.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tether::ssa {

class Builder {
public:
    Builder(type::TypeContext& tc, DiagnosticEmitter& diag,
            InternTable& intern, Arena& arena)
        : tc_(tc), diag_(diag), intern_(intern), arena_(arena) {}

    // Lower an entire module. Returns the SSA module.
    Module lower_module(const ast::Module& m);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;
    Arena&              arena_;

    // The module being built.
    Module* mod_ = nullptr;

    // The function being built.
    Function* fn_ = nullptr;

    // The current block being emitted to.
    BlockId current_block_ = kInvalidBlock;

    // The current mem token (threaded through memory ops).
    ValueId current_mem_ = kInvalidValue;

    // Whether we're inside an unsafe block.
    bool in_unsafe_ = false;

    // Local variable slots: AST binding name -> SSA ValueId.
    std::unordered_map<StrId, ValueId> locals_;

    // Arena name -> ArenaId.
    std::unordered_map<StrId, ArenaId> arena_ids_;

    // Struct definitions: name -> list of (field name, field type).
    struct StructDef {
        StrId name;
        std::vector<std::pair<StrId, type::TypePtr>> fields;
    };
    std::unordered_map<StrId, StructDef> struct_defs_;

    // Enum definitions: name -> list of (variant name, payload types).
    struct EnumDef {
        StrId name;
        std::vector<std::pair<StrId, std::vector<type::TypePtr>>> variants;
        // True if all variants have no payload (enum is just a tag).
        bool all_variants_empty = true;
    };
    std::unordered_map<StrId, EnumDef> enum_defs_;

    // Trait/impl tracking for method dispatch.
    // Map from (type name, method name) -> mangled function name.
    // When we see x.method(args), we look up the type of x and find
    // the concrete function that implements that method.
    struct MethodKey {
        StrId type_name;
        StrId method_name;
        bool operator==(const MethodKey& o) const {
            return type_name == o.type_name && method_name == o.method_name;
        }
    };
    struct MethodKeyHash {
        size_t operator()(const MethodKey& k) const {
            return k.type_name * 31 + k.method_name;
        }
    };
    std::unordered_map<MethodKey, StrId, MethodKeyHash> method_table_;

    // Map from type name -> list of methods (for type inference).
    std::unordered_map<StrId, std::vector<StrId>> type_methods_;

    // ---- Helpers ----
    ValueId fresh_value() {
        if (!fn_) return kInvalidValue;
        return fn_->next_value++;
    }
    BlockId fresh_block() {
        if (!fn_) return kInvalidBlock;
        BlockId id = fn_->next_block++;
        fn_->blocks.push_back({});
        fn_->blocks.back().id = id;
        return id;
    }

    // Append an instruction to the current block. Returns the
    // instruction's result ValueId (or kInvalidValue if the
    // instruction has no result).
    ValueId emit(Instruction inst);

    // Emit a constant and return its ValueId.
    ValueId emit_const_int(uint64_t value, type::TypePtr ty);
    ValueId emit_const_bool(bool value);
    ValueId emit_const_float(double value, type::TypePtr ty);

    // Emit a binary operation. Inserts the instruction and returns
    // the result ValueId.
    ValueId emit_binary(ast::BinaryOp op, ValueId lhs, ValueId rhs,
                        type::TypePtr ty, SourceRange loc);

    // Switch to a different block. Sets current_block_ and updates
    // current_mem_ to the new block's entry mem (which must have been
    // set by the predecessor).
    void set_block(BlockId b);

    // Add a predecessor edge.
    void add_pred(BlockId block, BlockId pred);

    // ---- Module lowering ----
    void lower_item(const ast::Item& item);
    void lower_fn(const ast::Item& item);
    void lower_extern(const ast::Item& item);
    void lower_struct(const ast::Item& item);

    // ---- Block / statement / expression lowering ----
    void lower_block(const ast::Block& b);
    void lower_stmt(const ast::Stmt& s);
    ValueId lower_expr(const ast::Expr& e);

    // Resolve an AST type to a type::Type.
    type::TypePtr resolve_ast_type(ast::TypePtr t);
};

} // namespace tether::ssa
