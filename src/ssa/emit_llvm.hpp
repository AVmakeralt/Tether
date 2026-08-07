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
//
// v0.9 (zero-overhead FFI): this emitter no longer widens integers
// to i64. Function signatures use the declared types (i32 stays i32,
// f32 stays float), calls look up the callee's signature and emit
// matching argument types, extern symbols are referenced by their
// bare name (no `_tether_` mangling), and calling conventions are
// propagated to `define`/`declare`/`call`. The result is that a
// Tether function calling `extern "C" fn printf` lowers to a direct
// `call i32 (i8*, ...) @printf(...)` with no wrapper — exactly the
// "FFI as a type/ABI lowering system" architecture from the design
// document.

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

    // The module currently being emitted. Set by `emit()`.
    const Module* mod_ = nullptr;

    std::string out_;
    uint32_t    next_reg_    = 1;
    uint32_t    next_string_ = 0;

    // Per-function: SSA ValueId -> LLVM register name.
    std::unordered_map<ValueId, std::string> reg_map_;

    // Per-function: SSA ValueId -> its type::TypePtr. Populated as
    // instructions are emitted; used to insert the correct cast when
    // a value flows into a use site that expects a different type
    // (e.g. an i64-typed integer literal being passed to an i32
    // parameter, or returned from an i32 function).
    std::unordered_map<ValueId, type::TypePtr> value_types_;

    // Per-function: SSA BlockId -> LLVM label name.
    std::unordered_map<BlockId, std::string> block_labels_;

    // String literal globals.
    std::unordered_map<StrId, std::string> strings_;

    // Struct type definitions already emitted (so we don't emit
    // `%struct.Foo = type { ... }` twice).
    std::unordered_map<StrId, bool> emitted_structs_;

    // Struct field layouts: name -> list of field LLVM types.
    // Populated from `mod_->struct_layouts` at module-emit time.
    std::unordered_map<StrId, const Module::StructLayout*> struct_layouts_;

    // ---- Helpers ----
    std::string fresh_reg() { return "%r" + std::to_string(next_reg_++); }
    std::string fresh_string() {
        return "@.str." + std::to_string(next_string_++);
    }
    std::string llvm_type(type::TypePtr t) { return tc_.render_llvm(t); }
    std::string reg_name(ValueId v);

    // Map a CallConv to its LLVM calling-convention token. Returns
    // the empty string for the default C convention (LLVM omits
    // `ccc`), so callers can concatenate unconditionally.
    static std::string call_conv_token(CallConv cc);

    // Look up the function or extern declaration with the given
    // interned name. Returns nullptr if not found.
    const Function*  lookup_function(StrId name) const;
    const ExternDecl* lookup_extern(StrId name) const;

    // Look up the struct layout by name. Returns nullptr if not found.
    const Module::StructLayout* lookup_struct(StrId name) const;

    // Get or create the LLVM label for an SSA block.
    std::string block_label(BlockId b);

    // Return the type of the value referenced by `v`, or nullptr if
    // unknown. Looks it up in `value_types_` first, then falls back
    // to scanning the function for the defining instruction (used
    // for forward references in phi nodes).
    type::TypePtr value_type(ValueId v) const;

    // Register the type of an SSA value (called when the value is
    // defined).
    void record_type(ValueId v, type::TypePtr t) {
        if (v != kInvalidValue && t) value_types_[v] = t;
    }

    // Coerce a value to the target type, emitting a cast instruction
    // if needed. Returns the (possibly new) register name holding the
    // coerced value. Handles:
    //   - integer widening  (i32 -> i64):  sext/zext
    //   - integer narrowing (i64 -> i32): trunc
    //   - integer same-width signed/unsigned change: no-op
    //   - same type: no-op
    // For mismatched non-integer types, returns the original
    // register unchanged (the IR may be invalid, but that's a
    // compiler bug to fix elsewhere, not a runtime concern).
    std::string coerce_to(std::string reg, type::TypePtr from,
                          type::TypePtr to);

    void emit_line(std::string s) { out_ += "  "; out_ += s; out_ += '\n'; }
    void emit_raw(std::string s) { out_ += s; out_ += '\n'; }

    // Emit a struct type definition at module scope. Recursively
    // emits any nested struct types.
    void emit_struct_decl(StrId name);

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
        value_types_.clear();
        block_labels_.clear();
    }
};

} // namespace tether::ssa
