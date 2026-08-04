// ssa/emit_llvm.cpp — SSA → LLVM IR text lowering with proper basic blocks

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

std::string LlvmEmitter::block_label(BlockId b) {
    auto it = block_labels_.find(b);
    if (it != block_labels_.end()) return it->second;
    std::string l = "bb" + std::to_string(b);
    block_labels_[b] = l;
    return l;
}

std::string LlvmEmitter::emit(const Module& mod) {
    out_.clear();
    out_ += "; Tether SSA → LLVM IR (v0.4)\n";
    out_ += "; Source module: ";
    out_ += intern_.get(mod.module_name);
    out_ += "\n\n";

    // Emit extern declarations.
    for (const auto& ext : mod.externs) {
        out_ += "declare ";
        out_ += ext.return_type ? llvm_type(ext.return_type) : "void";
        out_ += ext.return_type ? tc_.render_llvm_attrs(ext.return_type) : "";
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

void LlvmEmitter::emit_function(const Function& fn) {
    std::string name = std::string(intern_.get(fn.name));
    std::string mangled = "_tether_" + name;

    // Resolve return type — all integers lower to i64 for v0.4.
    type::TypePtr ret_type = fn.return_type;
    if (ret_type && tc_.is_integer(ret_type)) {
        ret_type = tc_.i64();
    }
    std::string ret_ty = ret_type ? llvm_type(ret_type) : "void";
    // Return type attributes (for ref returns).
    std::string ret_attrs = ret_type ? tc_.render_llvm_attrs(ret_type) : "";

    out_ += "define ";
    out_ += ret_ty;
    out_ += ret_attrs;
    out_ += " @";
    out_ += mangled;
    out_ += "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out_ += ", ";
        // Resolve param type — integers lower to i64.
        type::TypePtr pt = i < fn.param_types.size() ? fn.param_types[i] : tc_.i64();
        if (pt && tc_.is_integer(pt)) pt = tc_.i64();
        std::string pty = pt ? llvm_type(pt) : "i64";
        std::string pattrs = pt ? tc_.render_llvm_attrs(pt) : "";
        std::string arg = "%arg" + std::to_string(i);
        reg_map_[fn.params[i]] = arg;
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
    // Skip empty blocks (they're dead code). The entry block has no
    // label; subsequent blocks get one.
    bool first = true;
    for (const auto& block : fn.blocks) {
        // Skip dead blocks (no instructions, or only unreachable).
        if (block.instructions.empty()) continue;
        if (!first) {
            out_ += block_label(block.id);
            out_ += ":\n";
        }
        first = false;
        emit_block(block, fn);
    }

    // Ensure there's a terminator.
    out_ += "  ret ";
    out_ += ret_ty;
    if (!tc_.is_void(ret_type)) out_ += " 0";
    out_ += "\n";
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
        case Opcode::ConstInt: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = add i64 " + std::to_string(inst.int_data) + ", 0");
            break;
        }
        case Opcode::ConstFloat: {
            std::string r = reg_name(inst.result);
            std::ostringstream oss;
            oss << r << " = fadd double " << inst.float_data << ", 0.0";
            emit_line(oss.str());
            break;
        }
        case Opcode::ConstBool: {
            std::string r = reg_name(inst.result);
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
            }
            break;
        case Opcode::ConstArray:
            break;

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
            emit_line(r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }
        case Opcode::Neg: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = sub i64 0, " + reg_name(inst.operands[0]));
            break;
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
            emit_line(r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }
        case Opcode::Not: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = xor i64 " + reg_name(inst.operands[0]) + ", -1");
            break;
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
            emit_line(r + " = " + op + " i64 " +
                      reg_name(inst.operands[0]) + ", " +
                      reg_name(inst.operands[1]));
            break;
        }

        // ---- Memory ----
        case Opcode::Alloc: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = alloca i64");
            break;
        }
        case Opcode::Load: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = load i64, i64* " + reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Store: {
            emit_line("store i64 " + reg_name(inst.operands[1]) +
                      ", i64* " + reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Borrow:
            // Refs are just pointers in LLVM.
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
            }
            break;
        case Opcode::Move:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
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
                emit_line("ret i64 " + reg_name(inst.operands[0]));
            } else {
                emit_line("ret void");
            }
            break;
        }
        case Opcode::Unreachable:
            emit_line("unreachable");
            break;

        case Opcode::Phi: {
            // Emit: %r = phi i64 [ %v1, %bb1 ], [ %v2, %bb2 ]
            std::string r = reg_name(inst.result);
            std::string s = r + " = phi i64 ";
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i > 0) s += ", ";
                s += "[ " + reg_name(inst.operands[i]) + ", %" +
                     block_label(inst.blocks[i]) + " ]";
            }
            emit_line(s);
            break;
        }

        case Opcode::Call: {
            std::string r = reg_name(inst.result);
            std::string args;
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i) args += ", ";
                // v0.5: all args are i64 for now; attributes would
                // require knowing the callee's param types.
                args += "i64 " + reg_name(inst.operands[i]);
            }
            std::string callee = std::string(intern_.get(inst.str_data));
            emit_line(r + " = call i64 @_tether_" + callee + "(" + args + ")");
            break;
        }
        case Opcode::TailCall: {
            std::string args;
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (i) args += ", ";
                args += "i64 " + reg_name(inst.operands[i]);
            }
            std::string callee = std::string(intern_.get(inst.str_data));
            emit_line("tail call i64 @_tether_" + callee + "(" + args + ")");
            break;
        }

        case Opcode::Ref:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
            }
            break;
        case Opcode::Deref:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                reg_map_[inst.result] = reg_name(inst.operands[0]);
            }
            break;
        case Opcode::FieldAddr: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = getelementptr i64, i64* " +
                      reg_name(inst.operands[0]) + ", i64 0");
            break;
        }
        case Opcode::IndexAddr: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = getelementptr i64, i64* " +
                      reg_name(inst.operands[0]) + ", i64 " +
                      reg_name(inst.operands[1]));
            break;
        }

        case Opcode::BitCast: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = bitcast i64 " +
                      reg_name(inst.operands[0]) + " to i64");
            break;
        }
        case Opcode::ZExt: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = zext i64 " + reg_name(inst.operands[0]) + " to i64");
            break;
        }
        case Opcode::SExt: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = sext i64 " + reg_name(inst.operands[0]) + " to i64");
            break;
        }
        case Opcode::Trunc: {
            std::string r = reg_name(inst.result);
            emit_line(r + " = trunc i64 " + reg_name(inst.operands[0]) + " to i64");
            break;
        }

        case Opcode::BoundsCheck: {
            // Lower to: if idx >= len, trap.
            // v0.4: we don't have the length here. Emit a comment.
            emit_line("; bounds-check " + reg_name(inst.operands[0]));
            break;
        }
        case Opcode::Unsafe:
            // Marker only.
            break;

        case Opcode::StructConstruct: {
            // Allocate a struct on the stack, store each field.
            // %r = alloca %struct.Name
            // store i64 %v0, i64* %r.field0  (via GEP)
            // ...
            // For v0.4, structs are represented as a pointer to an
            // alloca'd region. We emit the struct as { i64, i64, ... }
            // (all fields widened to i64) for simplicity.
            std::string struct_name = std::string(intern_.get(inst.type->name));
            std::string r = reg_name(inst.result);
            emit_line(r + " = alloca { " +
                      std::string(inst.operands.size() > 0 ? "i64" : "") +
                      std::string(inst.operands.size() > 1 ? ", i64" : "") +
                      std::string(inst.operands.size() > 2 ? ", i64" : "") +
                      std::string(inst.operands.size() > 3 ? ", i64" : "") +
                      " }");
            // Store each field.
            for (size_t i = 0; i < inst.operands.size() && i < 8; ++i) {
                std::string field_ptr = fresh_reg();
                emit_line(field_ptr + " = getelementptr { " +
                          std::string(inst.operands.size() > 0 ? "i64" : "") +
                          std::string(inst.operands.size() > 1 ? ", i64" : "") +
                          std::string(inst.operands.size() > 2 ? ", i64" : "") +
                          std::string(inst.operands.size() > 3 ? ", i64" : "") +
                          " }, { " +
                          std::string(inst.operands.size() > 0 ? "i64" : "") +
                          std::string(inst.operands.size() > 1 ? ", i64" : "") +
                          std::string(inst.operands.size() > 2 ? ", i64" : "") +
                          std::string(inst.operands.size() > 3 ? ", i64" : "") +
                          " }* " + r + ", i64 0, i32 " + std::to_string(i));
                emit_line("store i64 " + reg_name(inst.operands[i]) +
                          ", i64* " + field_ptr);
            }
            break;
        }
        case Opcode::StructField: {
            // Load field at index from the struct pointer.
            // %r = getelementptr { i64, i64 }, { i64, i64 }* %base, i64 0, i32 N
            // %r2 = load i64, i64* %r
            std::string field_ptr = fresh_reg();
            // v0.4: assume 2-field struct for the GEP type. A proper
            // implementation would track the struct layout.
            emit_line(field_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      reg_name(inst.operands[0]) + ", i64 0, i32 " +
                      std::to_string(inst.field_index));
            std::string r = reg_name(inst.result);
            emit_line(r + " = load i64, i64* " + field_ptr);
            break;
        }
        case Opcode::EnumConstruct: {
            // Enums are represented as { i64 tag, i64 payload }.
            // v0.4: only single-payload or no-payload variants.
            std::string r = reg_name(inst.result);
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
                emit_line("store i64 " + reg_name(inst.operands[0]) +
                          ", i64* " + payload_ptr);
            }
            break;
        }
        case Opcode::EnumGetTag: {
            // Load the tag field (index 0).
            std::string tag_ptr = fresh_reg();
            emit_line(tag_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      reg_name(inst.operands[0]) + ", i64 0, i32 0");
            std::string r = reg_name(inst.result);
            emit_line(r + " = load i64, i64* " + tag_ptr);
            break;
        }
        case Opcode::EnumGetPayload: {
            // Load the payload field (index 1).
            std::string payload_ptr = fresh_reg();
            emit_line(payload_ptr + " = getelementptr { i64, i64 }, { i64, i64 }* " +
                      reg_name(inst.operands[0]) + ", i64 0, i32 1");
            std::string r = reg_name(inst.result);
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
