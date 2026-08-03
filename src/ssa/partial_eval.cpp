// ssa/partial_eval.cpp — partial evaluation implementation

#include "ssa/partial_eval.hpp"

namespace tether::ssa {

std::optional<uint64_t> PartialEvaluator::evaluate(
    const Function& fn, const std::vector<uint64_t>& args) {
    if (fn.blocks.empty()) return std::nullopt;
    fn_ = &fn;
    values_.clear();

    // Bind arguments.
    for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        values_[fn.params[i]] = args[i];
    }

    // Execute starting from the entry block.
    BlockId b = fn.entry;
    int iterations = 0;
    while (b != kInvalidBlock && iterations < 10000) {
        BlockId next = execute_block(b);
        b = next;
        ++iterations;
    }

    // Look for a Ret instruction and return its operand.
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.opcode == Opcode::Ret && !inst.operands.empty()) {
                return lookup(inst.operands[0]);
            }
        }
    }
    return std::nullopt;
}

BlockId PartialEvaluator::execute_block(BlockId b) {
    if (b >= fn_->blocks.size()) return kInvalidBlock;
    const auto& block = fn_->blocks[b];
    for (const auto& inst : block.instructions) {
        if (!eval_instruction(inst)) {
            // Can't evaluate — give up.
            return kInvalidBlock;
        }
        if (inst.opcode == Opcode::Ret) return kInvalidBlock;
        if (inst.opcode == Opcode::Br && !inst.blocks.empty()) {
            return inst.blocks[0];
        }
        if (inst.opcode == Opcode::CondBr && !inst.operands.empty()) {
            uint64_t cond = lookup(inst.operands[0]);
            return cond ? (inst.blocks.size() > 0 ? inst.blocks[0] : kInvalidBlock)
                        : (inst.blocks.size() > 1 ? inst.blocks[1] : kInvalidBlock);
        }
    }
    return kInvalidBlock;
}

bool PartialEvaluator::eval_instruction(const Instruction& inst) {
    switch (inst.opcode) {
        case Opcode::ConstInt:
        case Opcode::ConstBool:
            if (inst.result != kInvalidValue) {
                values_[inst.result] = inst.int_data;
            }
            return true;
        case Opcode::ConstFloat:
            // Floats not supported in v0.3 eval.
            return false;
        case Opcode::ConstStr:
        case Opcode::ConstNull:
            return false;

        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod:
        case Opcode::And: case Opcode::Or: case Opcode::Xor:
        case Opcode::Shl: case Opcode::Shr:
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
        case Opcode::Gt: case Opcode::Le: case Opcode::Ge: {
            if (inst.operands.size() != 2) return false;
            uint64_t a = lookup(inst.operands[0]);
            uint64_t b = lookup(inst.operands[1]);
            uint64_t result = 0;
            switch (inst.opcode) {
                case Opcode::Add: result = a + b; break;
                case Opcode::Sub: result = a - b; break;
                case Opcode::Mul: result = a * b; break;
                case Opcode::Div: result = b ? a / b : 0; break;
                case Opcode::Mod: result = b ? a % b : 0; break;
                case Opcode::And: result = a & b; break;
                case Opcode::Or:  result = a | b; break;
                case Opcode::Xor: result = a ^ b; break;
                case Opcode::Shl: result = a << b; break;
                case Opcode::Shr: result = a >> b; break;
                case Opcode::Eq:  result = (a == b); break;
                case Opcode::Ne:  result = (a != b); break;
                case Opcode::Lt:  result = (static_cast<int64_t>(a) < static_cast<int64_t>(b)); break;
                case Opcode::Gt:  result = (static_cast<int64_t>(a) > static_cast<int64_t>(b)); break;
                case Opcode::Le:  result = (static_cast<int64_t>(a) <= static_cast<int64_t>(b)); break;
                case Opcode::Ge:  result = (static_cast<int64_t>(a) >= static_cast<int64_t>(b)); break;
                default: return false;
            }
            if (inst.result != kInvalidValue) {
                values_[inst.result] = result;
            }
            return true;
        }
        case Opcode::Neg: {
            if (inst.operands.size() != 1) return false;
            uint64_t a = lookup(inst.operands[0]);
            values_[inst.result] = -static_cast<int64_t>(a);
            return true;
        }
        case Opcode::Not: {
            if (inst.operands.size() != 1) return false;
            uint64_t a = lookup(inst.operands[0]);
            values_[inst.result] = ~a;
            return true;
        }

        // Memory ops — not supported in comptime (pure evaluation).
        case Opcode::Alloc: case Opcode::Load: case Opcode::Store:
        case Opcode::Borrow: case Opcode::Move: case Opcode::Drop:
        case Opcode::MemPhi:
            return false;

        // Control flow — handled by execute_block.
        case Opcode::Br: case Opcode::CondBr: case Opcode::Switch:
        case Opcode::Ret: case Opcode::Unreachable:
            return true;

        case Opcode::Phi:
            // v0.3: would need to know which predecessor we came from.
            return false;

        case Opcode::Call:
            // v0.3: would recurse into the callee. For now, give up.
            return false;
        case Opcode::TailCall:
            return false;

        case Opcode::Ref: case Opcode::Deref:
        case Opcode::FieldAddr: case Opcode::IndexAddr:
            return false;

        case Opcode::BitCast:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                values_[inst.result] = lookup(inst.operands[0]);
                return true;
            }
            return false;
        case Opcode::ZExt: case Opcode::SExt: case Opcode::Trunc:
            if (inst.result != kInvalidValue && !inst.operands.empty()) {
                values_[inst.result] = lookup(inst.operands[0]);
                return true;
            }
            return false;

        case Opcode::BoundsCheck:
            // No-op in comptime (no runtime trap).
            return true;
        case Opcode::Unsafe:
            return true;
        case Opcode::ConstArray:
            return false;
    }
    return false;
}

} // namespace tether::ssa
