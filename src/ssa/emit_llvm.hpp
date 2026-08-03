// ssa/emit_llvm.hpp — SSA → LLVM IR text lowering
//
// This is the final stage of the pipeline. It takes an optimized SSA
// module and emits LLVM IR text (.ll) that can be assembled with clang
// or llc.
//
// The lowering is mostly one-to-one: each SSA opcode maps to an LLVM
// instruction. The Tether-specific opcodes (Borrow, Move, Drop,
// BoundsCheck, Unsafe) are resolved here:
//
//   - Borrow: lowered to a no-op (the ref is just a pointer in LLVM)
//   - Move:   lowered to a no-op (ownership is verified at SSA level)
//   - Drop:   lowered to a call to the arena's free function
//   - BoundsCheck: lowered to an icmp + cond_br + trap
//   - Unsafe: lowered to nothing (marker only)

#pragma once

#include "ssa/node.hpp"
#include "diagnostics/diagnostics.hpp"
#include "types/types.hpp"

#include <string>

namespace tether::ssa {

class LlvmEmitter {
public:
    LlvmEmitter(type::TypeContext& tc, DiagnosticEmitter& diag,
                InternTable& intern)
        : tc_(tc), diag_(diag), intern_(intern) {}

    // Emit an entire SSA module as LLVM IR text.
    std::string emit(const Module& mod);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;

    std::string out_;
    uint32_t    next_reg_    = 1;
    uint32_t    next_label_  = 1;
    uint32_t    next_string_ = 0;

    // Per-function: SSA ValueId -> LLVM register name.
    std::unordered_map<ValueId, std::string> reg_map_;

    // String literal globals.
    std::unordered_map<StrId, std::string> strings_;

    // Helpers.
    std::string fresh_reg() { return "%r" + std::to_string(next_reg_++); }
    std::string fresh_label() { return "L" + std::to_string(next_label_++); }
    std::string fresh_string() {
        return "@.str." + std::to_string(next_string_++);
    }
    std::string llvm_type(type::TypePtr t) { return tc_.render_llvm(t); }
    std::string reg_name(ValueId v);

    void emit_line(std::string s) { out_ += s; out_ += '\n'; }

    // Emit a function.
    void emit_function(const Function& fn);

    // Emit a single instruction. Returns the LLVM register holding the
    // instruction's result (or empty string if no result).
    std::string emit_instruction(const Instruction& inst, const Function& fn);

    // Emit a string literal global.
    void emit_string_global(StrId str_id, const std::string& name);

    // Reset per-function state.
    void reset_function_state() {
        next_reg_ = 1;
        next_label_ = 1;
        reg_map_.clear();
    }
};

} // namespace tether::ssa
