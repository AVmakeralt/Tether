// llvm/emit.hpp — LLVM IR text emitter
//
// Emits LLVM IR text (.ll) directly from the AST. No LLVM library
// dependency — the output is a .ll file that can be assembled with
// `llc` or compiled to an object with `clang`.
//
// v0.2 supports:
//   - Integer / float / bool / void primitives
//   - Function definitions and declarations
//   - Local variables (via alloca + load/store)
//   - Binary arithmetic on integers
//   - Function calls (including extern)
//   - Return, branches (if/else), loops (while)
//   - String literals as global constants
//
// v0.2 does NOT yet emit code for:
//   - Struct / enum / union values (their types are emitted as opaque
//     structs, but construction and field access are not supported)
//   - Pattern matching (lowered to if-chains in a future version)
//   - Generics (require monomorphization)
//   - Closures / spawn / await
//   - The `rewrite` language feature

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace tether::llvm {

class Emitter {
public:
    Emitter(type::TypeContext& tc, DiagnosticEmitter& diag,
            InternTable& intern, Arena& arena)
        : tc_(tc), diag_(diag), intern_(intern), arena_(arena) {}

    // Emit a module. Returns the LLVM IR text.
    std::string emit_module(const ast::Module& m);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;
    Arena&              arena_;

    // The output buffer.
    std::string out_;

    // Counters for generating unique SSA register names.
    uint32_t next_reg_ = 1;
    uint32_t next_label_ = 1;
    uint32_t next_string_ = 0;

    // String literal globals: bytes -> global name.
    std::unordered_map<StrId, std::string> strings_;

    // Per-function: local variable slots (alloca name -> type).
    struct LocalSlot {
        std::string alloca_name;
        type::TypePtr type;
    };
    std::unordered_map<StrId, LocalSlot> locals_;

    // Per-function: the entry block's alloca section. We emit allocas
    // at the start of the function, before any other instructions.
    std::string entry_allocas_;

    // The current block's instructions.
    std::string body_;

    // Whether we're inside a block that has already terminated (e.g.
    // after a return or unconditional branch).
    bool terminated_ = false;

    // The current function's return type. Used by `return` statements
    // to emit the correct `ret` instruction.
    type::TypePtr current_ret_type_ = nullptr;

    // ---- Helpers ----
    std::string fresh_reg() {
        return "%r" + std::to_string(next_reg_++);
    }
    std::string fresh_label() {
        return "L" + std::to_string(next_label_++);
    }
    std::string fresh_string() {
        return "@.str." + std::to_string(next_string_++);
    }

    void emit_line(std::string s) { body_ += s; body_ += '\n'; }
    void emit_line_to(std::string& target, std::string s) {
        target += s;
        target += '\n';
    }

    std::string llvm_type(type::TypePtr t) {
        return tc_.render_llvm(t);
    }

    // Emit a module-level item (function, struct declaration, etc.).
    void emit_item(const ast::Item& item);

    // Emit a function. `is_decl` true means emit only a declaration.
    void emit_fn(const ast::Item& item, bool is_decl);

    // Emit a struct type definition.
    void emit_struct_type(const ast::Item& item);

    // Emit a block. Returns the SSA register holding the block's
    // value (or empty string if the block's type is void).
    std::string emit_block(ast::BlockPtr b);

    // Emit a statement.
    void emit_stmt(ast::StmtPtr s);

    // Emit an expression. Returns the SSA register holding the
    // expression's value.
    std::string emit_expr(ast::ExprPtr e);

    // Emit a string literal as a global and return its name.
    std::string emit_string_literal(StrId str_id);

    // Reset per-function state.
    void reset_function_state();
};

} // namespace tether::llvm
