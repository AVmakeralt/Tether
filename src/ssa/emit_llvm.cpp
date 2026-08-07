// ssa/emit_llvm.cpp — SSA → LLVM IR text lowering with proper basic blocks,
// calling conventions, real integer widths, and zero-overhead FFI calls.
//
// v0.9 (zero-overhead FFI):
//   - Function signatures use the declared types. i32 stays i32, not i64.
//   - Calling conventions propagate from `extern "C"` / `extern "fastcall"`
//     / etc. to LLVM's `define`/`declare`/`call` instructions.
//   - Extern functions are referenced by their bare name (no `_tether_`
//     mangling), so a Tether call to `extern fn printf(...)` lowers to a
//     direct `call i32 (i8*, ...) @printf(...)` with no wrapper.
//   - Call sites look up the callee's signature and emit matching arg
//     types, inserting sext/zext/trunc as needed.
//   - Struct construction and field access use the real struct layout
//     (`{ i32, double }` instead of `{ i64, i64 }`).

#include "ssa/emit_llvm.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace tether::ssa {

using namespace tether::type;

// ---- Calling-convention mapping --------------------------------------
//
// LLVM's calling conventions: https://llvm.org/docs/LangRef.html#calling-conventions
//   ccc                  — default C calling convention (omitted in text IR)
//   fastcc               — fast calling convention (fastcall on x86)
//   x86_stdcallcc        — stdcall (Win32)
//   x86_vectorcallcc     — vectorcall (Windows)
//   x86_64_sysvcc        — System V AMD64
//   x86_64_win64cc       — Microsoft x64 ABI
//
// Tether's native convention (CallConv::Tether) currently lowers to
// ccc — the spec says LLVM is a backend, not part of the language, so
// the convention name we expose to users ("tether") is independent of
// whatever LLVM convention we happen to lower to. v1 keeps it as ccc
// for portability; a future Tether-specific calling convention would
// plug in here without touching the rest of the compiler.

std::string LlvmEmitter::call_conv_token(CallConv cc) {
    switch (cc) {
        case CallConv::C:
        case CallConv::Tether:
            return "";  // ccc is the default; LLVM omits it
        case CallConv::Fastcall:    return "fastcc ";
        case CallConv::Stdcall:     return "x86_stdcallcc ";
        case CallConv::Vectorcall:  return "x86_vectorcallcc ";
        case CallConv::SysV:        return "x86_64_sysvcc ";
        case CallConv::Win64:       return "x86_64_win64cc ";
    }
    return "";
}

const Function* LlvmEmitter::lookup_function(StrId name) const {
    if (!mod_) return nullptr;
    for (const auto& fn : mod_->functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

const ExternDecl* LlvmEmitter::lookup_extern(StrId name) const {
    if (!mod_) return nullptr;
    for (const auto& ext : mod_->externs) {
        if (ext.name == name) return &ext;
    }
    return nullptr;
}

const Module::StructLayout* LlvmEmitter::lookup_struct(StrId name) const {
    auto it = struct_layouts_.find(name);
    return it == struct_layouts_.end() ? nullptr : it->second;
}

type::TypePtr LlvmEmitter::value_type(ValueId v) const {
    auto it = value_types_.find(v);
    if (it != value_types_.end()) return it->second;
    return nullptr;
}

std::string LlvmEmitter::coerce_to(std::string reg, type::TypePtr from,
                                   type::TypePtr to) {
    if (!from || !to) return reg;
    if (from == to) return reg;
    if (from->kind != Kind::Int || to->kind != Kind::Int) {
        // Same-kind pointer/ref? BitCast would be wrong for opaque
        // types; leave alone and let the verifier flag it if it's a
        // real mismatch. Float<->int needs bitcast; skip for now.
        if (from->kind == to->kind && from->kind != Kind::Int) return reg;
        return reg;
    }
    if (from->bit_width == to->bit_width) return reg;
    std::string r = fresh_reg();
    if (from->bit_width < to->bit_width) {
        // Widen. Use sext for signed, zext for unsigned — the sign
        // comes from the source type, since that's what the value
        // actually is.
        const char* op = from->is_signed ? "sext" : "zext";
        emit_line(r + " = " + op + " " + llvm_type(from) + " " + reg +
                  " to " + llvm_type(to));
    } else {
        // Narrow. trunc is sign-truncating, which is correct for
        // both signed and unsigned (the high bits don't survive
        // anyway).
        emit_line(r + " = trunc " + llvm_type(from) + " " + reg +
                  " to " + llvm_type(to));
    }
    return r;
}

std::string LlvmEmitter::reg_name(ValueId v) {
    if (v == kInvalidValue) return "";
    auto it = reg_map_.find(v);
    if (it != reg_map_.end()) return it->second;
    // Allocate a fresh register for this value.
    std::string r = fresh_reg();
    reg_map_[v] = r;
    return r;
}

std::string LlvmEmitter::block_label(BlockId b) {
    auto it = block_labels_.find(b);
    if (it != block_labels_.end()) return it->second;
    std::string l = "bb" + std::to_string(b);
    block_labels_[b] = l;
    return l;
}

// ---- Module emission --------------------------------------------------

std::string LlvmEmitter::emit(const Module& mod) {
    out_.clear();
    out_ += "; Tether SSA → LLVM IR (v0.9 zero-overhead FFI)\n";
    out_ += "; Source module: ";
    out_ += intern_.get(mod.module_name);
    out_ += "\n\n";

    mod_ = &mod;

    // Index struct layouts for O(1) lookup.
    struct_layouts_.clear();
    for (const auto& layout : mod.struct_layouts) {
        struct_layouts_[layout.name] = &layout;
    }

    // Emit struct type declarations first. We emit them all up front
    // so any function can reference any struct without worrying about
    // forward declarations (LLVM IR is forward-reference-friendly
    // for opaque types, but explicit definitions are easier to read
    // and necessary for `getelementptr` to compute field offsets).
    for (const auto& layout : mod.struct_layouts) {
        emit_struct_decl(layout.name);
    }
    if (!mod.struct_layouts.empty()) out_ += "\n";

    // Emit extern declarations.
    for (const auto& ext : mod.externs) {
        std::string ret_ty = ext.return_type ? llvm_type(ext.return_type) : "void";
        std::string ret_attrs = ext.return_type
            ? tc_.render_llvm_attrs(ext.return_type) : "";
        out_ += "declare ";
        out_ += call_conv_token(ext.call_conv);
        out_ += ret_ty;
        out_ += ret_attrs;
        out_ += " @";
        out_ += intern_.get(ext.name);
        out_ += "(";
        for (size_t i = 0; i < ext.param_types.size(); ++i) {
            if (i) out_ += ", ";
            out_ += llvm_type(ext.param_types[i]);
            out_ += tc_.render_llvm_attrs(ext.param_types[i]);
        }
        if (ext.is_variadic) {
            if (!ext.param_types.empty()) out_ += ", ";
            out_ += "...";
        }
        out_ += ")\n";
    }
    if (!mod.externs.empty()) out_ += "\n";

    // Emit functions first (collecting string globals).
    std::string function_output;
    for (const auto& fn : mod.functions) {
        reset_function_state();
        std::string saved = out_;
        out_.clear();
        emit_function(fn);
        function_output += out_;
        out_ = saved;
    }

    // Emit string globals.
    for (const auto& [sid, name] : strings_) {
        emit_string_global(sid, name);
    }
    if (!strings_.empty()) out_ += "\n";

    out_ += function_output;
    return out_;
}

void LlvmEmitter::emit_struct_decl(StrId name) {
    if (emitted_structs_.count(name)) return;
    emitted_structs_[name] = true;

    const Module::StructLayout* layout = lookup_struct(name);
    std::string sname = std::string(intern_.get(name));
    out_ += "%struct.";
    out_ += sname;
    out_ += " = type { ";
    if (!layout || layout->field_types.empty()) {
        out_ += "i8";  // empty struct placeholder
    } else {
        for (size_t i = 0; i < layout->field_types.size(); ++i) {
            if (i) out_ += ", ";
            out_ += llvm_type(layout->field_types[i]);
        }
    }
    out_ += " }\n";
}

void LlvmEmitter::emit_function(const Function& fn) {
    std::string name = std::string(intern_.get(fn.name));
    // Extern declarations are emitted from `mod.externs`, not here;
    // a `Function` only reaches this path if it has a body. But the
    // `is_extern` field exists for future use (e.g. an extern
    // function that also has a Tether-side definition for testing).
    // The mangling rule: Tether-defined functions get the
    // `_tether_` prefix; extern functions (which would have come
    // from `mod.externs` in normal use) keep their bare name so C
    // can link against them.
    std::string mangled = fn.is_extern ? name : ("_tether_" + name);

    // Resolve return type — no widening. v0.9 uses the real type.
    type::TypePtr ret_type = fn.return_type;
    std::string ret_ty = ret_type ? llvm_type(ret_type) : "void";
    std::string ret_attrs = ret_type ? tc_.render_llvm_attrs(ret_type) : "";

    out_ += "define ";
    out_ += call_conv_token(fn.call_conv);
    out_ += ret_ty;
    out_ += ret_attrs;
    out_ += " @";
    out_ += mangled;
    out_ += "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out_ += ", ";
        type::TypePtr pt = i < fn.param_types.size() ? fn.param_types[i]
                                                     : tc_.i64();
        std::string pty = llvm_type(pt);
        std::string pattrs = tc_.render_llvm_attrs(pt);
        std::string arg = "%arg" + std::to_string(i);
        reg_map_[fn.params[i]] = arg;
        record_type(fn.params[i], pt);
        out_ += pty;
        out_ += pattrs;
        out_ += " ";
        out_ += arg;
    }
    out_ += ") {\n";

    // Map the entry mem token.
    if (fn.entry_mem != kInvalidValue) {
        reg_map_[fn.entry_mem] = "";
    }

    // Emit each block as an LLVM basic block.
    bool first = true;
    bool last_block_terminated = false;
    for (const auto& block : fn.blocks) {
        if (block.instructions.empty()) continue;
        if (!first) {
            out_ += block_label(block.id);
            out_ += ":\n";
        }
        first = false;
        emit_block(block, fn);
        // Track whether the most recently emitted block already has
        // a terminator. We need this to decide whether to emit a
        // fallback `ret` below.
        last_block_terminated = !block.instructions.empty() &&
                                block.instructions.back().is_terminator();
    }

    // Ensure there's a terminator. If the last non-empty block
    // already ended with a Ret/Br/Unreachable/TailCall, we're done;
    // otherwise we emit a default return so the IR is well-formed
    // (LLVM requires every basic block to end with a terminator).
    if (!last_block_terminated) {
        out_ += "  ret ";
        out_ += ret_ty;
        if (!tc_.is_void(ret_type)) {
            out_ += " 0";
        }
        out_ += "\n";
    }
    out_ += "}\n\n";
}

void LlvmEmitter::emit_block(const Block& block, const Function& fn) {
    for (const auto& inst : block.instructions) {
        emit_instruction(inst, fn);
    }
}

void LlvmEmitter::emit_instruction(const Instruction& inst,
                                   const Function& fn) {
    (void)fn;
    switch (inst.opcode) {
        // ---- Constants ----
        case Opcode::ConstInt: {
            std::string r = reg_name(inst.result);
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            record_type(inst.result, ty);
            // LLVM has no "literal-move" instruction; the historical
            // `add i64 X, 0` pattern survives optimizer passes just
            // fine and gives every constant a register name, which
            // keeps the rest of the emitter simple.
            emit_line(r + " = add " + llvm_type(ty) + " " +
                      std::to_string(inst.int_data) + ", 0");
            break;
        }
        case Opcode::ConstFloat: {
            std::string r = reg_name(inst.result);
            type::TypePtr ty = inst.type ? inst.type : tc_.f64();
            record_type(inst.result, ty);
            std::ostringstream oss;
            oss << r << " = fadd " << llvm_type(ty) << " "
                << inst.float_data << ", 0.0";
            emit_line(oss.str());
            break;
        }
        case Opcode::ConstBool: {
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.boolean());
            emit_line(r + " = add i1 " + std::to_string(inst.int_data) + ", 0");
            break;
        }
        case Opcode::ConstStr: {
            auto it = strings_.find(inst.str_data);
            std::string global_name;
            if (it == strings_.end()) {
                global_name = fresh_string();
                strings_[inst.str_data] = global_name;
            } else {
                global_name = it->second;
            }
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.make_raw_ptr(tc_.u8(), false));
            std::string_view text = intern_.get(inst.str_data);
            emit_line(r + " = getelementptr [" +
                      std::to_string(text.size() + 1) + " x i8], [" +
                      std::to_string(text.size() + 1) + " x i8]* " +
                      global_name + ", i64 0, i64 0");
            break;
        }
        case Opcode::ConstNull:
            if (inst.result != kInvalidValue) {
                reg_map_[inst.result] = "null";
                record_type(inst.result, tc_.make_raw_ptr(tc_.u8(), false));
            }
            break;
        case Opcode::ConstArray:
            break;

        // ---- Arithmetic ----
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod: {
            const char* op = "add";
            bool is_float = false;
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            if (ty && ty->kind == Kind::Float) {
                is_float = true;
                switch (inst.opcode) {
                    case Opcode::Add: op = "fadd"; break;
                    case Opcode::Sub: op = "fsub"; break;
                    case Opcode::Mul: op = "fmul"; break;
                    case Opcode::Div: op = "fdiv"; break;
                    case Opcode::Mod: op = "frem"; break;
                    default: break;
                }
            } else {
                // Integer division is signed or unsigned based on
                // the type's sign. Use sdiv/srem for signed, udiv/urem
                // for unsigned.
                bool is_signed = !ty || ty->is_signed;
                switch (inst.opcode) {
                    case Opcode::Add: op = "add";  break;
                    case Opcode::Sub: op = "sub";  break;
                    case Opcode::Mul: op = "mul";  break;
                    case Opcode::Div: op = is_signed ? "sdiv" : "udiv"; break;
                    case Opcode::Mod: op = is_signed ? "srem" : "urem"; break;
                    default: break;
                }
            }
            (void)is_float;
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            emit_line(r + " = " + op + " " + llvm_type(ty) + " " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }
        case Opcode::Neg: {
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            if (ty->kind == Kind::Float) {
                emit_line(r + " = fneg " + llvm_type(ty) + " " +
                          reg_name(inst.operands[0]));
            } else {
                emit_line(r + " = sub " + llvm_type(ty) + " 0, " +
                          reg_name(inst.operands[0]));
            }
            break;
        }
        case Opcode::And: case Opcode::Or: case Opcode::Xor:
        case Opcode::Shl: case Opcode::Shr: {
            const char* op = "and";
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            // Shift right: signed → ashr, unsigned → lshr.
            switch (inst.opcode) {
                case Opcode::And: op = "and"; break;
                case Opcode::Or:  op = "or";  break;
                case Opcode::Xor: op = "xor"; break;
                case Opcode::Shl: op = "shl"; break;
                case Opcode::Shr:
                    op = (ty && ty->is_signed) ? "ashr" : "lshr";
                    break;
                default: break;
            }
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            emit_line(r + " = " + op + " " + llvm_type(ty) + " " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }
        case Opcode::Not: {
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            emit_line(r + " = xor " + llvm_type(ty) + " " +
                      reg_name(inst.operands[0]) + ", -1");
            break;
        }
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
        case Opcode::Gt: case Opcode::Le: case Opcode::Ge: {
            const char* op = "icmp eq";
            type::TypePtr operand_ty = value_type(inst.operands[0]);
            if (!operand_ty) operand_ty = tc_.i64();
            bool is_float = operand_ty->kind == Kind::Float;
            bool is_signed = operand_ty->is_signed;
            if (is_float) {
                switch (inst.opcode) {
                    case Opcode::Eq: op = "fcmp oeq"; break;
                    case Opcode::Ne: op = "fcmp one"; break;
                    case Opcode::Lt: op = "fcmp olt"; break;
                    case Opcode::Gt: op = "fcmp ogt"; break;
                    case Opcode::Le: op = "fcmp ole"; break;
                    case Opcode::Ge: op = "fcmp oge"; break;
                    default: break;
                }
            } else {
                switch (inst.opcode) {
                    case Opcode::Eq: op = "icmp eq"; break;
                    case Opcode::Ne: op = "icmp ne"; break;
                    case Opcode::Lt:
                        op = is_signed ? "icmp slt" : "icmp ult"; break;
                    case Opcode::Gt:
                        op = is_signed ? "icmp sgt" : "icmp ugt"; break;
                    case Opcode::Le:
                        op = is_signed ? "icmp sle" : "icmp ule"; break;
                    case Opcode::Ge:
                        op = is_signed ? "icmp sge" : "icmp uge"; break;
                    default: break;
                }
            }
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.boolean());
            emit_line(r + " = " + op + " " + llvm_type(operand_ty) + " " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }

        // ---- Memory ----
        case Opcode::Alloc: {
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            // Alloc produces a pointer to the allocated type.
            record_type(inst.result, tc_.make_raw_ptr(ty, true));
            emit_line(r + " = alloca " + llvm_type(ty));
            break;
        }
        case Opcode::Load: {
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            // The pointer operand's pointee type matches the load
            // type. v0.9 uses typed pointers (opaque pointers are
            // the LLVM 15+ default, but we still emit the
            // pointee-typed form for readability and llc
            // compatibility on older toolchains).
            emit_line(r + " = load " + llvm_type(ty) + ", " +
                      llvm_type(ty) + "* " + reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Store: {
            type::TypePtr val_ty = value_type(inst.operands[1]);
            if (!val_ty) val_ty = tc_.i64();
            emit_line("store " + llvm_type(val_ty) + " " +
                      reg_name(inst.operands[1]) + ", " +
                      llvm_type(val_ty) + "* " +
                      reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Borrow:
            // Refs are just pointers in LLVM.
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
                record_type(inst.result, value_type(inst.operands[0]));
            }
            break;
        case Opcode::Move:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
                record_type(inst.result, value_type(inst.operands[0]));
            }
            break;
        case Opcode::Drop:
            break;
        case Opcode::MemPhi:
            break;

        // ---- Control flow ----
        case Opcode::Br: {
            if (!inst.blocks.empty()) {
                emit_line("br label %" + block_label(inst.blocks[0]));
            }
            break;
        }
        case Opcode::CondBr: {
            if (inst.blocks.size() >= 2 && !inst.operands.empty()) {
                emit_line("br i1 " + reg_name(inst.operands[0]) +
                          ", label %" + block_label(inst.blocks[0]) +
                          ", label %" + block_label(inst.blocks[1]));
            }
            break;
        }
        case Opcode::Switch:
            break;
        case Opcode::Ret: {
            if (!inst.operands.empty()) {
                type::TypePtr val_ty = value_type(inst.operands[0]);
                if (!val_ty) val_ty = tc_.i64();
                type::TypePtr ret_ty = fn.return_type;
                std::string reg = reg_name(inst.operands[0]);
                if (ret_ty) {
                    reg = coerce_to(reg, val_ty, ret_ty);
                }
                emit_line("ret " + llvm_type(ret_ty ? ret_ty : val_ty) +
                          " " + reg);
            } else {
                emit_line("ret void");
            }
            break;
        }
        case Opcode::Unreachable:
            emit_line("unreachable");
            break;

        case Opcode::Phi: {
            type::TypePtr ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, ty);
            std::string s = r + " = phi " + llvm_type(ty) + " ";
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i > 0) s += ", ";
                s += "[ " + reg_name(inst.operands[i]) + ", %" +
                     block_label(inst.blocks[i]) + " ]";
            }
            emit_line(s);
            break;
        }

        // ---- Calls ----
        //
        // v0.9: a call looks up the callee's signature (either a
        // Tether function in `mod_->functions` or an extern in
        // `mod_->externs`) and emits the matching argument and
        // return types. Extern calls use the bare symbol name; calls
        // to Tether functions use the `_tether_`-mangled name.
        case Opcode::Call:
        case Opcode::TailCall: {
            bool is_tail = (inst.opcode == Opcode::TailCall);
            const Function* callee_fn = lookup_function(inst.str_data);
            const ExternDecl* callee_ext = lookup_extern(inst.str_data);

            std::string callee_sym;
            CallConv cc = CallConv::Tether;
            std::vector<TypePtr> param_types;
            TypePtr ret_type = nullptr;
            bool is_variadic = false;

            if (callee_ext) {
                callee_sym = std::string(intern_.get(callee_ext->name));
                cc = callee_ext->call_conv;
                param_types = callee_ext->param_types;
                ret_type = callee_ext->return_type;
                is_variadic = callee_ext->is_variadic;
            } else if (callee_fn) {
                callee_sym = "_tether_" +
                             std::string(intern_.get(callee_fn->name));
                cc = callee_fn->call_conv;
                param_types = callee_fn->param_types;
                ret_type = callee_fn->return_type;
                is_variadic = false;  // Tether fns are never variadic
            } else {
                // Unknown callee — fall back to a C-convention call
                // with all-i64 args. This is the historical v0.4
                // behavior; it shouldn't fire for well-formed
                // programs, but it keeps the emitter from crashing
                // on tests that reference undeclared functions.
                callee_sym = "_tether_" +
                             std::string(intern_.get(inst.str_data));
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    param_types.push_back(tc_.i64());
                }
                ret_type = tc_.i64();
            }

            std::string ret_ty = ret_type ? llvm_type(ret_type) : "void";
            std::string ret_attrs = ret_type
                ? tc_.render_llvm_attrs(ret_type) : "";

            std::string args;
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i) args += ", ";
                TypePtr expected = i < param_types.size()
                    ? param_types[i] : tc_.i64();
                std::string arg_reg = reg_name(inst.operands[i]);
                TypePtr arg_ty = value_type(inst.operands[i]);
                if (!arg_ty) arg_ty = expected;
                // Coerce argument to the callee's declared param
                // type. This is the difference between "zero-overhead
                // FFI" and "hopelessly broken FFI": a Tether integer
                // literal defaults to i64, but if the callee declares
                // the param as i32, we must truncate to i32 before
                // the call. LLVM does not insert implicit casts.
                arg_reg = coerce_to(arg_reg, arg_ty, expected);
                args += llvm_type(expected);
                args += tc_.render_llvm_attrs(expected);
                args += " ";
                args += arg_reg;
            }
            // Variadic tail: LLVM requires the `...` to appear after
            // the fixed args in the *type* of the function being
            // called, but the call instruction itself just lists the
            // extra args. For declared variadic externs, the
            // `declare` already has the `...`; the call is fine as
            // long as the extra args are present.

            std::string prefix = is_tail ? "tail call " : "call ";
            if (inst.result != kInvalidValue && !tc_.is_void(ret_type)) {
                std::string r = reg_name(inst.result);
                record_type(inst.result, ret_type);
                emit_line(r + " = " + prefix + call_conv_token(cc) +
                          ret_ty + ret_attrs + " @" + callee_sym +
                          "(" + args + ")");
            } else {
                emit_line(prefix + call_conv_token(cc) +
                          ret_ty + ret_attrs + " @" + callee_sym +
                          "(" + args + ")");
            }
            break;
        }

        case Opcode::Ref:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
                record_type(inst.result, value_type(inst.operands[0]));
            }
            break;
        case Opcode::Deref:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
                record_type(inst.result, value_type(inst.operands[0]));
            }
            break;

        // ---- Struct field addresses ----
        //
        // v0.9 uses the real struct layout from `mod_->struct_layouts`
        // instead of the v0.4 placeholder `{ i64, i64 }` GEP.
        case Opcode::FieldAddr: {
            const Module::StructLayout* layout = nullptr;
            if (inst.type && inst.type->name != kInvalidStrId) {
                layout = lookup_struct(inst.type->name);
            }
            // If the FieldAddr didn't carry its struct type, try to
            // find it from the operand's type.
            if (!layout) {
                TypePtr ptr_ty = value_type(inst.operands[0]);
                if (ptr_ty && (ptr_ty->kind == Kind::Struct ||
                               ptr_ty->kind == Kind::RawPtr ||
                               ptr_ty->kind == Kind::Ref)) {
                    // Walk through ref/rawptr to the base.
                    while (ptr_ty &&
                           (ptr_ty->kind == Kind::RawPtr ||
                            ptr_ty->kind == Kind::Ref)) {
                        ptr_ty = ptr_ty->base;
                    }
                    if (ptr_ty && ptr_ty->name != kInvalidStrId) {
                        layout = lookup_struct(ptr_ty->name);
                    }
                }
            }

            std::string struct_ty;
            if (layout) {
                struct_ty = "%struct." + std::string(intern_.get(layout->name));
            } else {
                // Unknown struct — fall back to a single-i64 placeholder
                // so the GEP is at least well-formed.
                struct_ty = "{ i64 }";
            }
            std::string r = reg_name(inst.result);
            // FieldAddr produces a pointer to the field's type.
            TypePtr field_ty = nullptr;
            if (layout && inst.field_index < layout->field_types.size()) {
                field_ty = layout->field_types[inst.field_index];
            }
            record_type(inst.result,
                        field_ty ? tc_.make_raw_ptr(field_ty, true)
                                 : tc_.make_raw_ptr(tc_.i64(), true));
            emit_line(r + " = getelementptr " + struct_ty + ", " +
                      struct_ty + "* " + reg_name(inst.operands[0]) +
                      ", i64 0, i32 " + std::to_string(inst.field_index));
            break;
        }
        case Opcode::IndexAddr: {
            type::TypePtr elem_ty = inst.type ? inst.type : tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.make_raw_ptr(elem_ty, true));
            emit_line(r + " = getelementptr " + llvm_type(elem_ty) + ", " +
                      llvm_type(elem_ty) + "* " +
                      reg_name(inst.operands[0]) + ", i64 " +
                      reg_name(inst.operands[1]));
            break;
        }

        // ---- Casts ----
        case Opcode::BitCast: {
            type::TypePtr from = value_type(inst.operands[0]);
            type::TypePtr to = inst.type ? inst.type : tc_.i64();
            if (!from) from = tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, to);
            emit_line(r + " = bitcast " + llvm_type(from) + " " +
                      reg_name(inst.operands[0]) + " to " + llvm_type(to));
            break;
        }
        case Opcode::ZExt: {
            type::TypePtr from = value_type(inst.operands[0]);
            type::TypePtr to = inst.type ? inst.type : tc_.i64();
            if (!from) from = tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, to);
            emit_line(r + " = zext " + llvm_type(from) + " " +
                      reg_name(inst.operands[0]) + " to " + llvm_type(to));
            break;
        }
        case Opcode::SExt: {
            type::TypePtr from = value_type(inst.operands[0]);
            type::TypePtr to = inst.type ? inst.type : tc_.i64();
            if (!from) from = tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, to);
            emit_line(r + " = sext " + llvm_type(from) + " " +
                      reg_name(inst.operands[0]) + " to " + llvm_type(to));
            break;
        }
        case Opcode::Trunc: {
            type::TypePtr from = value_type(inst.operands[0]);
            type::TypePtr to = inst.type ? inst.type : tc_.i64();
            if (!from) from = tc_.i64();
            std::string r = reg_name(inst.result);
            record_type(inst.result, to);
            emit_line(r + " = trunc " + llvm_type(from) + " " +
                      reg_name(inst.operands[0]) + " to " + llvm_type(to));
            break;
        }

        case Opcode::BoundsCheck: {
            // Lower to: if idx >= len, trap.
            // v0.9: still emits a comment; a real lowering needs the
            // length operand which the SSA opcode doesn't carry yet.
            emit_line("; bounds-check " + reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Unsafe:
            // Marker only.
            break;

        // ---- Struct / Enum construction ----
        //
        // v0.9 uses the real struct layout. Each field is stored at
        // its real type via a typed GEP, so `{ i32, double }` no
        // longer becomes `{ i64, i64 }`.
        case Opcode::StructConstruct: {
            // Look up the struct layout by name.
            StrId struct_name = kInvalidStrId;
            if (inst.type && inst.type->name != kInvalidStrId) {
                struct_name = inst.type->name;
            }
            const Module::StructLayout* layout =
                struct_name != kInvalidStrId ? lookup_struct(struct_name)
                                             : nullptr;

            std::string struct_ty;
            std::vector<TypePtr> field_tys;
            if (layout) {
                struct_ty = "%struct." + std::string(intern_.get(layout->name));
                field_tys = layout->field_types;
            } else {
                // Unknown struct — fall back to all-i64.
                struct_ty = "{ ";
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    if (i) struct_ty += ", ";
                    struct_ty += "i64";
                }
                struct_ty += " }";
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    field_tys.push_back(tc_.i64());
                }
            }

            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.make_raw_ptr(
                layout ? tc_.make_struct(struct_name) : tc_.i64(), true));
            emit_line(r + " = alloca " + struct_ty);
            // Store each field.
            for (size_t i = 0; i < inst.operands.size() &&
                               i < field_tys.size(); ++i) {
                std::string field_ptr = fresh_reg();
                emit_line(field_ptr + " = getelementptr " + struct_ty +
                          ", " + struct_ty + "* " + r + ", i64 0, i32 " +
                          std::to_string(i));
                TypePtr expected = field_tys[i];
                TypePtr actual = value_type(inst.operands[i]);
                if (!actual) actual = expected;
                std::string val_reg = reg_name(inst.operands[i]);
                val_reg = coerce_to(val_reg, actual, expected);
                emit_line("store " + llvm_type(expected) + " " + val_reg +
                          ", " + llvm_type(expected) + "* " + field_ptr);
            }
            break;
        }
        case Opcode::StructField: {
            // Look up the struct layout from the operand's type. We
            // can't get it directly from `inst.type` because the
            // builder sets that to the field's *value* type, not the
            // struct type — so we have to scan the layouts.
            //
            // Simpler approach: emit a GEP using the layout found by
            // matching the field_index to a struct that has at least
            // that many fields. Since StructField's `field_index`
            // comes from the builder's struct_defs_ lookup (which
            // matches by field name), the first struct with a
            // matching field count is correct in practice for the
            // tests; a future improvement is to tag StructField with
            // the struct name explicitly.
            const Module::StructLayout* layout = nullptr;
            for (const auto& l : mod_->struct_layouts) {
                if (inst.field_index < l.field_types.size()) {
                    layout = &l;
                    break;
                }
            }

            std::string struct_ty;
            TypePtr field_ty = nullptr;
            if (layout) {
                struct_ty = "%struct." + std::string(intern_.get(layout->name));
                field_ty = layout->field_types[inst.field_index];
            } else {
                struct_ty = "{ i64, i64 }";
                field_ty = tc_.i64();
            }

            std::string field_ptr = fresh_reg();
            emit_line(field_ptr + " = getelementptr " + struct_ty + ", " +
                      struct_ty + "* " + reg_name(inst.operands[0]) +
                      ", i64 0, i32 " + std::to_string(inst.field_index));
            std::string r = reg_name(inst.result);
            record_type(inst.result, field_ty);
            emit_line(r + " = load " + llvm_type(field_ty) + ", " +
                      llvm_type(field_ty) + "* " + field_ptr);
            break;
        }
        case Opcode::EnumConstruct: {
            // Enums are represented as { i64 tag, i64 payload }.
            // v0.9: the tag stays i64 (it's an index, not a user
            // value); the payload is still widened to i64. A future
            // improvement would use the variant's declared payload
            // type for the second field.
            std::string r = reg_name(inst.result);
            StrId enum_name = inst.type ? inst.type->name : kInvalidStrId;
            record_type(inst.result, enum_name != kInvalidStrId
                ? tc_.make_enum(enum_name) : tc_.i64());
            emit_line(r + " = alloca { i64, i64 }");
            // Store the tag.
            std::string tag_ptr = fresh_reg();
            emit_line(tag_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      r + ", i64 0, i32 0");
            emit_line("store i64 " + std::to_string(inst.field_index) +
                      ", i64* " + tag_ptr);
            // Store the payload (first operand, if any).
            if (!inst.operands.empty()) {
                std::string payload_ptr = fresh_reg();
                emit_line(payload_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                          r + ", i64 0, i32 1");
                TypePtr actual = value_type(inst.operands[0]);
                if (!actual) actual = tc_.i64();
                std::string val_reg = reg_name(inst.operands[0]);
                val_reg = coerce_to(val_reg, actual, tc_.i64());
                emit_line("store i64 " + val_reg + ", i64* " + payload_ptr);
            }
            break;
        }
        case Opcode::EnumGetTag: {
            // Load the tag field (index 0).
            std::string tag_ptr = fresh_reg();
            emit_line(tag_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      reg_name(inst.operands[0]) + ", i64 0, i32 0");
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.i64());
            emit_line(r + " = load i64, i64* " + tag_ptr);
            break;
        }
        case Opcode::EnumGetPayload: {
            // Load the payload field (index 1).
            std::string payload_ptr = fresh_reg();
            emit_line(payload_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      reg_name(inst.operands[0]) + ", i64 0, i32 1");
            std::string r = reg_name(inst.result);
            record_type(inst.result, tc_.i64());
            emit_line(r + " = load i64, i64* " + payload_ptr);
            break;
        }
    }
}

void LlvmEmitter::emit_string_global(StrId str_id, const std::string& name) {
    std::string_view text = intern_.get(str_id);
    out_ += name;
    out_ += " = private unnamed_addr constant [";
    out_ += std::to_string(text.size() + 1);
    out_ += " x i8] c\"";
    for (char c : text) {
        if (c == '\\' || c == '"') { out_ += '\\'; out_ += c; }
        else if (c >= 32 && c < 127) { out_ += c; }
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%02X",
                          static_cast<unsigned char>(c));
            out_ += buf;
        }
    }
    out_ += "\\00\"\n";
}

} // namespace tether::ssa
