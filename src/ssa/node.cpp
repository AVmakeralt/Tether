// ssa/node.cpp — opcode name table

#include "node.hpp"

namespace tether::ssa {

const char* opcode_name(Opcode op) {
    switch (op) {
        case Opcode::ConstInt:    return "const-int";
        case Opcode::ConstFloat:  return "const-float";
        case Opcode::ConstStr:    return "const-str";
        case Opcode::ConstBool:   return "const-bool";
        case Opcode::ConstNull:   return "const-null";
        case Opcode::Add:         return "add";
        case Opcode::Sub:         return "sub";
        case Opcode::Mul:         return "mul";
        case Opcode::Div:         return "div";
        case Opcode::Mod:         return "mod";
        case Opcode::Neg:         return "neg";
        case Opcode::Eq:          return "eq";
        case Opcode::Ne:          return "ne";
        case Opcode::Lt:          return "lt";
        case Opcode::Gt:          return "gt";
        case Opcode::Le:          return "le";
        case Opcode::Ge:          return "ge";
        case Opcode::And:         return "and";
        case Opcode::Or:          return "or";
        case Opcode::Xor:         return "xor";
        case Opcode::Not:         return "not";
        case Opcode::Shl:         return "shl";
        case Opcode::Shr:         return "shr";
        case Opcode::Alloc:       return "alloc";
        case Opcode::Load:        return "load";
        case Opcode::Store:       return "store";
        case Opcode::Borrow:      return "borrow";
        case Opcode::Move:        return "move";
        case Opcode::Drop:        return "drop";
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
    }
    return "?";
}

} // namespace tether::ssa
