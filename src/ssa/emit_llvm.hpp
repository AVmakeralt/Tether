// ssa/emit_llvm.hpp — SSA → LLVM IR text lowering
//
// Emits proper LLVM basic blocks with labels and branch instructions.
// Each SSA block becomes an LLVM basic block. Phi nodes become LLVM
// phi instructions with incoming (value, block) pairs.
//
// Tether-specific opcodes are resolved here:
//   - Borrow: no-op (refs are pointers in LLVM)
//   - Move:   no-op (ownership verified at SSA level)
//   - Drop:   call to arena free function
//   - BoundsCheck: icmp + cond_br + trap
//   - Unsafe: marker only (no LLVM instruction)

#pragma once

#include "ssa/node.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <string>
#include <unordered_map>

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
    uint32_t    next_string_ = 0;

    // Per-function: SSA ValueId -> LLVM register name.
    std::unordered_map<ValueId, std::string> reg_map_;

    // Per-function: SSA BlockId -> LLVM label name.
    std::unordered_map<BlockId, std::string> block_labels_;

    // String literal globals.
    std::unordered_map<StrId, std::string> strings_;

    // Struct type definitions already emitted.
    std::unordered_map<StrId, bool> emitted_structs_;

    // Struct field layouts: name -> list of field LLVM types.
    std::unordered_map<StrId, std::vector<std::string>> struct_layouts_;

    // ---- Helpers ----
    std::string fresh_reg() { return "%r" + std::to_string(next_reg_++); }
    std::string fresh_string() {
        return "@.str." + std::to_string(next_string_++);
    }
    std::string llvm_type(type::TypePtr t) { return tc_.render_llvm(t); }
    std::string reg_name(ValueId v);

    // Get or create the LLVM label for an SSA block.
    std::string block_label(BlockId b);

    void emit_line(std::string s) { out_ += "  "; out_ += s; out_ += '\n'; }
    void emit_raw(std::string s) { out_ += s; out_ += '\n'; }

    // Emit a function.
    void emit_function(const Function& fn);

    // Emit a single block as an LLVM basic block.
    void emit_block(const Block& block, const Function& fn);

    // Emit a single instruction. Appends to out_.
    void emit_instruction(const Instruction& inst, const Function& fn);

    // Emit a string literal global.
    void emit_string_global(StrId str_id, const std::string& name);

    // Reset per-function state.
    void reset_function_state() {
        next_reg_ = 1;
        reg_map_.clear();
        block_labels_.clear();
    }
};

} // namespace tether::ssa
