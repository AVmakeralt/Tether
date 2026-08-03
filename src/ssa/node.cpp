// ssa/node.cpp — SSA opcode names + rendering

#include "node.hpp"

#include <sstream>
#include <string>

namespace tether::ssa {

const char* opcode_name(Opcode op) {
    switch (op) {
        case Opcode::ConstInt:    return "const-int";
        case Opcode::ConstFloat:  return "const-float";
        case Opcode::ConstStr:    return "const-str";
        case Opcode::ConstBool:   return "const-bool";
        case Opcode::ConstNull:   return "const-null";
        case Opcode::ConstArray:  return "const-array";
        case Opcode::Add:         return "add";
        case Opcode::Sub:         return "sub";
        case Opcode::Mul:         return "mul";
        case Opcode::Div:         return "div";
        case Opcode::Mod:         return "mod";
        case Opcode::Neg:         return "neg";
        case Opcode::And:         return "and";
        case Opcode::Or:          return "or";
        case Opcode::Xor:         return "xor";
        case Opcode::Not:         return "not";
        case Opcode::Shl:         return "shl";
        case Opcode::Shr:         return "shr";
        case Opcode::Eq:          return "eq";
        case Opcode::Ne:          return "ne";
        case Opcode::Lt:          return "lt";
        case Opcode::Gt:          return "gt";
        case Opcode::Le:          return "le";
        case Opcode::Ge:          return "ge";
        case Opcode::Alloc:       return "alloc";
        case Opcode::Load:        return "load";
        case Opcode::Store:       return "store";
        case Opcode::Borrow:      return "borrow";
        case Opcode::Move:        return "move";
        case Opcode::Drop:        return "drop";
        case Opcode::MemPhi:      return "mem-phi";
        case Opcode::Br:          return "br";
        case Opcode::CondBr:      return "cond-br";
        case Opcode::Switch:      return "switch";
        case Opcode::Ret:         return "ret";
        case Opcode::Unreachable: return "unreachable";
        case Opcode::Phi:         return "phi";
        case Opcode::Call:        return "call";
        case Opcode::TailCall:    return "tail-call";
        case Opcode::Ref:         return "ref";
        case Opcode::Deref:       return "deref";
        case Opcode::FieldAddr:   return "field-addr";
        case Opcode::IndexAddr:   return "index-addr";
        case Opcode::BitCast:     return "bitcast";
        case Opcode::ZExt:        return "zext";
        case Opcode::SExt:        return "sext";
        case Opcode::Trunc:       return "trunc";
        case Opcode::BoundsCheck: return "bounds-check";
        case Opcode::Unsafe:      return "unsafe";
    }
    return "?";
}

namespace {

std::string render_value(ValueId v) {
    if (v == kInvalidValue) return "<invalid>";
    return "%v" + std::to_string(v);
}

std::string render_operands(const std::vector<ValueId>& ops) {
    std::string s;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i) s += ", ";
        s += render_value(ops[i]);
    }
    return s;
}

std::string render_instruction(const Instruction& inst,
                               type::TypeContext& tc) {
    std::ostringstream out;
    if (inst.result != kInvalidValue) {
        out << render_value(inst.result);
        out << " : ";
        out << tc.render(inst.type);
        out << " = ";
    }
    out << opcode_name(inst.opcode);

    if (inst.opcode == Opcode::ConstInt) {
        out << " " << inst.int_data;
    } else if (inst.opcode == Opcode::ConstFloat) {
        out << " " << inst.float_data;
    } else if (inst.opcode == Opcode::ConstBool) {
        out << " " << (inst.int_data ? "true" : "false");
    } else if (inst.opcode == Opcode::ConstStr) {
        // str_data is an interned id; we don't have the InternTable
        // here. Just show the id.
        out << " str#" << inst.str_data;
    } else if (inst.opcode == Opcode::ConstNull) {
        out << " null";
    } else if (!inst.operands.empty()) {
        out << " " << render_operands(inst.operands);
    }

    if (inst.mem_in != kInvalidValue) {
        out << " [mem " << render_value(inst.mem_in) << "]";
    }
    if (inst.mem_out != kInvalidValue) {
        out << " -> mem " << render_value(inst.mem_out);
    }
    if (inst.arena != kNoArena) {
        out << " arena#" << inst.arena;
    }
    if (inst.region != kStaticRegion) {
        out << " region#" << inst.region;
    }
    if (inst.is_unsafe) {
        out << " [unsafe]";
    }
    if (inst.opcode == Opcode::Br && !inst.blocks.empty()) {
        out << " block%" << inst.blocks[0];
    } else if (inst.opcode == Opcode::CondBr && inst.blocks.size() >= 2) {
        out << " then block%" << inst.blocks[0]
            << " else block%" << inst.blocks[1];
    } else if (inst.opcode == Opcode::Ret) {
        // ret value is in operands[0] (or void if empty)
    }
    return out.str();
}

} // namespace

std::string render_function(const Function& fn,
                            type::TypeContext& tc) {
    std::ostringstream out;
    out << "fn " << tc.intern().get(fn.name);
    out << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out << ", ";
        out << render_value(fn.params[i]) << " : "
            << tc.render(fn.param_types[i]);
    }
    out << ") -> ";
    out << (fn.return_type ? tc.render(fn.return_type) : "void");
    out << " {\n";
    out << "  entry_mem = " << render_value(fn.entry_mem) << "\n";

    for (const auto& block : fn.blocks) {
        out << "block%" << block.id << ":\n";
        if (!block.predecessors.empty()) {
            out << "  ; preds:";
            for (BlockId p : block.predecessors) out << " block%" << p;
            out << "\n";
        }
        for (const auto& inst : block.instructions) {
            out << "  " << render_instruction(inst, tc) << "\n";
        }
        out << "\n";
    }
    out << "}\n";
    return out.str();
}

std::string render_module(const Module& mod, type::TypeContext& tc) {
    std::ostringstream out;
    out << "; Tether SSA module: ";
    out << tc.intern().get(mod.module_name) << "\n\n";

    if (!mod.arenas.empty()) {
        out << "; arenas:\n";
        for (size_t i = 0; i < mod.arenas.size(); ++i) {
            out << ";   arena#" << i << " = "
                << tc.intern().get(mod.arenas[i]) << "\n";
        }
        out << "\n";
    }

    for (const auto& ext : mod.externs) {
        out << "extern " << tc.intern().get(ext.name) << "(";
        for (size_t i = 0; i < ext.param_types.size(); ++i) {
            if (i) out << ", ";
            out << tc.render(ext.param_types[i]);
        }
        if (ext.is_variadic) {
            if (!ext.param_types.empty()) out << ", ";
            out << "...";
        }
        out << ") -> ";
        out << (ext.return_type ? tc.render(ext.return_type) : "void");
        out << "\n";
    }
    if (!mod.externs.empty()) out << "\n";

    for (const auto& g : mod.globals) {
        out << "global " << tc.intern().get(g.name) << " : "
            << tc.render(g.type);
        if (g.is_const) out << " const";
        out << "\n";
    }
    if (!mod.globals.empty()) out << "\n";

    for (const auto& fn : mod.functions) {
        out << render_function(fn, tc);
        out << "\n";
    }
    return out.str();
}

} // namespace tether::ssa
