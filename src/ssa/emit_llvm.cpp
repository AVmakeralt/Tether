// ssa/emit_llvm.cpp — SSA → LLVM IR text lowering

#include "ssa/emit_llvm.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace tether::ssa {

using namespace tether::type;

std::string LlvmEmitter::reg_name(ValueId v) {
    if (v == kInvalidValue) return "";
    auto it = reg_map_.find(v);
    if (it != reg_map_.end()) return it->second;
    // Allocate a fresh register for this value.
    std::string r = fresh_reg();
    reg_map_[v] = r;
    return r;
}

std::string LlvmEmitter::emit(const Module& mod) {
    out_.clear();
    out_ += "; Tether SSA → LLVM IR (v0.3)\n";
    out_ += "; Source module: ";
    out_ += intern_.get(mod.module_name);
    out_ += "\n\n";

    // Emit extern declarations.
    for (const auto& ext : mod.externs) {
        out_ += "declare ";
        out_ += ext.return_type ? llvm_type(ext.return_type) : "void";
        out_ += " @";
        out_ += intern_.get(ext.name);
        out_ += "(";
        for (size_t i = 0; i < ext.param_types.size(); ++i) {
            if (i) out_ += ", ";
            out_ += llvm_type(ext.param_types[i]);
        }
        if (ext.is_variadic) {
            if (!ext.param_types.empty()) out_ += ", ";
            out_ += "...";
        }
        out_ += ")\n";
    }
    if (!mod.externs.empty()) out_ += "\n";

    // Emit string globals (collected during function emission).
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

void LlvmEmitter::emit_function(const Function& fn) {
    std::string name = std::string(intern_.get(fn.name));
    std::string mangled = "_tether_" + name;

    // Resolve return type — all integers lower to i64 for v0.3.
    type::TypePtr ret_type = fn.return_type;
    if (ret_type && tc_.is_integer(ret_type)) {
        ret_type = tc_.i64();
    }
    std::string ret_ty = ret_type ? llvm_type(ret_type) : "void";

    out_ += "define ";
    out_ += ret_ty;
    out_ += " @";
    out_ += mangled;
    out_ += "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out_ += ", ";
        // Map param ValueId to %argN.
        std::string arg = "%arg" + std::to_string(i);
        reg_map_[fn.params[i]] = arg;
        out_ += "i64 ";  // v0.3: all params are i64
        out_ += arg;
    }
    out_ += ") {\n";
    out_ += "entry:\n";

    // Map the entry mem token to a placeholder (LLVM doesn't track
    // memory as a value — it's implicit).
    if (fn.entry_mem != kInvalidValue) {
        reg_map_[fn.entry_mem] = "";  // empty = "no register"
    }

    // Emit all blocks. v0.3: flatten all blocks into the entry block
    // for simplicity. A proper lowering would emit LLVM basic blocks
    // with branch instructions. For now, we emit block labels as
    // comments and flatten.
    bool first_block = true;
    for (const auto& block : fn.blocks) {
        if (!first_block) {
            out_ += "; block%";
            out_ += std::to_string(block.id);
            out_ += ":\n";
        }
        first_block = false;
        for (const auto& inst : block.instructions) {
            (void)emit_instruction(inst, fn);
        }
    }

    // Ensure terminator.
    out_ += "  ret ";
    out_ += ret_ty;
    if (!tc_.is_void(ret_type)) out_ += " 0";
    out_ += "\n";
    out_ += "}\n\n";
}

std::string LlvmEmitter::emit_instruction(const Instruction& inst,
                                          const Function& fn) {
    (void)fn;
    switch (inst.opcode) {
        case Opcode::ConstInt: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = add i64 " +
                      std::to_string(inst.int_data) + ", 0");
            return r;
        }
        case Opcode::ConstFloat: {
            std::string r = reg_name(inst.result);
            std::ostringstream oss;
            oss << "  " << r << " = fadd double " << inst.float_data
                << ", 0.0";
            emit_line(oss.str());
            return r;
        }
        case Opcode::ConstBool: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = add i1 " +
                      std::to_string(inst.int_data) + ", 0");
            return r;
        }
        case Opcode::ConstStr: {
            // Emit a global for the string if not already done.
            auto it = strings_.find(inst.str_data);
            std::string global_name;
            if (it == strings_.end()) {
                global_name = fresh_string();
                strings_[inst.str_data] = global_name;
            } else {
                global_name = it->second;
            }
            std::string r = reg_name(inst.result);
            std::string_view text = intern_.get(inst.str_data);
            emit_line("  " + r + " = getelementptr [" +
                      std::to_string(text.size() + 1) + " x i8], [" +
                      std::to_string(text.size() + 1) + " x i8]* " +
                      global_name + ", i64 0, i64 0");
            return r;
        }
        case Opcode::ConstNull:
            return "null";
        case Opcode::ConstArray:
            // v0.3: arrays not fully supported.
            return "";

        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod: {
            const char* op = "add";
            switch (inst.opcode) {
                case Opcode::Add: op = "add"; break;
                case Opcode::Sub: op = "sub"; break;
                case Opcode::Mul: op = "mul"; break;
                case Opcode::Div: op = "sdiv"; break;
                case Opcode::Mod: op = "srem"; break;
                default: break;
            }
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            return r;
        }
        case Opcode::Neg: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = sub i64 0, " +
                      reg_name(inst.operands[0]));
            return r;
        }
        case Opcode::And: case Opcode::Or: case Opcode::Xor:
        case Opcode::Shl: case Opcode::Shr: {
            const char* op = "and";
            switch (inst.opcode) {
                case Opcode::And: op = "and"; break;
                case Opcode::Or:  op = "or";  break;
                case Opcode::Xor: op = "xor"; break;
                case Opcode::Shl: op = "shl"; break;
                case Opcode::Shr: op = "ashr"; break;
                default: break;
            }
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            return r;
        }
        case Opcode::Not: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = xor i64 " +
                      reg_name(inst.operands[0]) + ", -1");
            return r;
        }
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
        case Opcode::Gt: case Opcode::Le: case Opcode::Ge: {
            const char* op = "icmp eq";
            switch (inst.opcode) {
                case Opcode::Eq: op = "icmp eq"; break;
                case Opcode::Ne: op = "icmp ne"; break;
                case Opcode::Lt: op = "icmp slt"; break;
                case Opcode::Gt: op = "icmp sgt"; break;
                case Opcode::Le: op = "icmp sle"; break;
                case Opcode::Ge: op = "icmp sge"; break;
                default: break;
            }
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            return r;
        }

        // ---- Memory ----
        case Opcode::Alloc: {
            // v0.3: lower to an alloca. Arena allocation would lower
            // to a call to the arena's bump function.
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = alloca i64");
            return r;
        }
        case Opcode::Load: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = load i64, i64* " +
                      reg_name(inst.operands[0]));
            return r;
        }
        case Opcode::Store: {
            emit_line("  store i64 " + reg_name(inst.operands[1]) +
                      ", i64* " + reg_name(inst.operands[0]));
            return "";
        }
        case Opcode::Borrow:
            // Refs are just pointers in LLVM.
            return reg_name(inst.operands[0]);
        case Opcode::Move:
            // Moves are no-ops at the LLVM level (ownership verified
            // at SSA level).
            return reg_name(inst.operands[0]);
        case Opcode::Drop:
            // v0.3: heap drop would be a free() call; arena drop is
            // handled at the arena's lifetime end.
            return "";
        case Opcode::MemPhi:
            // Memory phis are resolved by LLVM's own MemorySSA.
            return "";

        // ---- Control flow ----
        case Opcode::Br:
            emit_line("  br label %L" + std::to_string(inst.blocks[0]));
            return "";
        case Opcode::CondBr: {
            // v0.3: would emit proper LLVM basic blocks. For now,
            // flatten by assuming the condition is always true.
            emit_line("  ; condbr " + reg_name(inst.operands[0]));
            return "";
        }
        case Opcode::Switch:
            return "";
        case Opcode::Ret: {
            if (!inst.operands.empty()) {
                emit_line("  ret i64 " + reg_name(inst.operands[0]));
            } else {
                emit_line("  ret void");
            }
            return "";
        }
        case Opcode::Unreachable:
            emit_line("  unreachable");
            return "";

        case Opcode::Phi:
            // v0.3: would emit proper phi with incoming values.
            return reg_name(inst.operands.empty() ? kInvalidValue
                                                    : inst.operands[0]);

        case Opcode::Call: {
            std::string r = reg_name(inst.result);
            std::string args;
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i) args += ", ";
                args += "i64 " + reg_name(inst.operands[i]);
            }
            std::string callee = std::string(intern_.get(inst.str_data));
            emit_line("  " + r + " = call i64 @_tether_" + callee +
                      "(" + args + ")");
            return r;
        }
        case Opcode::TailCall: {
            std::string args;
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i) args += ", ";
                args += "i64 " + reg_name(inst.operands[i]);
            }
            std::string callee = std::string(intern_.get(inst.str_data));
            emit_line("  tail call i64 @_tether_" + callee +
                      "(" + args + ")");
            return "";
        }

        case Opcode::Ref:
            // Ref creates a pointer to a local. Lower to alloca.
            return reg_name(inst.operands[0]);
        case Opcode::Deref:
            return reg_name(inst.operands[0]);
        case Opcode::FieldAddr: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = getelementptr i64, i64* " +
                      reg_name(inst.operands[0]) + ", i64 0");
            return r;
        }
        case Opcode::IndexAddr: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = getelementptr i64, i64* " +
                      reg_name(inst.operands[0]) + ", i64 " +
                      reg_name(inst.operands[1]));
            return r;
        }

        case Opcode::BitCast: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = bitcast i64 " +
                      reg_name(inst.operands[0]) + " to i64");
            return r;
        }
        case Opcode::ZExt: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = zext i64 " +
                      reg_name(inst.operands[0]) + " to i64");
            return r;
        }
        case Opcode::SExt: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = sext i64 " +
                      reg_name(inst.operands[0]) + " to i64");
            return r;
        }
        case Opcode::Trunc: {
            std::string r = reg_name(inst.result);
            emit_line("  " + r + " = trunc i64 " +
                      reg_name(inst.operands[0]) + " to i64");
            return r;
        }

        case Opcode::BoundsCheck: {
            // Lower to: if idx >= len, trap.
            // v0.3: we don't have the length here (it's a slice field).
            // For now, emit a comment.
            emit_line("  ; bounds-check " + reg_name(inst.operands[0]));
            return "";
        }
        case Opcode::Unsafe:
            // Marker only — no LLVM equivalent.
            return "";
    }
    return "";
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
