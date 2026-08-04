// ssa/optimizer.cpp — SSA optimization passes

#include "ssa/optimizer.hpp"

#include <algorithm>
#include <unordered_map>

namespace tether::ssa {

using namespace tether::type;

uint32_t Optimizer::run(Module& mod) {
    uint32_t total_changes = 0;
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 100) {
        changed = false;
        for (auto& fn : mod.functions) {
            uint32_t c = 0;
            c += constant_folding(fn);
            c += common_subexpression_elimination(fn);
            c += dead_code_elimination(fn);
            c += remove_unreachable_blocks(fn);
            c += simplify_cfg(fn);
            if (c > 0) changed = true;
            total_changes += c;
        }
        ++iterations;
    }
    return total_changes;
}

bool Optimizer::is_pure(const Instruction& inst) const {
    switch (inst.opcode) {
        case Opcode::ConstInt:
        case Opcode::ConstFloat:
        case Opcode::ConstStr:
        case Opcode::ConstBool:
        case Opcode::ConstNull:
        case Opcode::ConstArray:
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod: case Opcode::Neg:
        case Opcode::And: case Opcode::Or: case Opcode::Xor:
        case Opcode::Not: case Opcode::Shl: case Opcode::Shr:
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
        case Opcode::Gt: case Opcode::Le: case Opcode::Ge:
        case Opcode::Phi: case Opcode::MemPhi:
        case Opcode::BitCast: case Opcode::ZExt:
        case Opcode::SExt: case Opcode::Trunc:
        case Opcode::FieldAddr: case Opcode::IndexAddr:
        case Opcode::Ref:
            return true;
        // Impure: memory ops, calls, terminators, bounds checks.
        case Opcode::Alloc: case Opcode::Load: case Opcode::Store:
        case Opcode::Borrow: case Opcode::Move: case Opcode::Drop:
        case Opcode::Br: case Opcode::CondBr: case Opcode::Switch:
        case Opcode::Ret: case Opcode::Unreachable:
        case Opcode::Call: case Opcode::TailCall:
        case Opcode::Deref: case Opcode::BoundsCheck:
        case Opcode::Unsafe:
            return false;
    }
    return false;
}

uint32_t Optimizer::constant_folding(Function& fn) {
    uint32_t changes = 0;
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            if (inst.operands.size() != 2) continue;
            // Find the defining instructions for the operands.
            // v0.3: we do a simple linear scan — for each operand, find
            // the last ConstInt/ConstBool that defines it in the same
            // block or an earlier block.
            auto find_const = [&](ValueId v, uint64_t& out) -> bool {
                if (v == kInvalidValue) return false;
                // Scan all blocks for a ConstInt/ConstBool with this result.
                for (auto& b : fn.blocks) {
                    for (auto& i : b.instructions) {
                        if (i.result == v &&
                            (i.opcode == Opcode::ConstInt ||
                             i.opcode == Opcode::ConstBool)) {
                            out = i.int_data;
                            return true;
                        }
                    }
                }
                return false;
            };
            uint64_t a, b;
            if (!find_const(inst.operands[0], a)) continue;
            if (!find_const(inst.operands[1], b)) continue;

            uint64_t result = 0;
            bool folded = true;
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
                case Opcode::Eq:  result = (a == b) ? 1 : 0; break;
                case Opcode::Ne:  result = (a != b) ? 1 : 0; break;
                case Opcode::Lt:  result = (static_cast<int64_t>(a) < static_cast<int64_t>(b)) ? 1 : 0; break;
                case Opcode::Gt:  result = (static_cast<int64_t>(a) > static_cast<int64_t>(b)) ? 1 : 0; break;
                case Opcode::Le:  result = (static_cast<int64_t>(a) <= static_cast<int64_t>(b)) ? 1 : 0; break;
                case Opcode::Ge:  result = (static_cast<int64_t>(a) >= static_cast<int64_t>(b)) ? 1 : 0; break;
                default: folded = false; break;
            }
            if (folded) {
                inst.opcode   = Opcode::ConstInt;
                inst.int_data = result;
                inst.operands.clear();
                ++changes;
            }
        }
    }
    return changes;
}

uint32_t Optimizer::common_subexpression_elimination(Function& fn) {
    uint32_t changes = 0;
    for (auto& block : fn.blocks) {
        // Map from (opcode, operands) -> result ValueId.
        std::unordered_map<uint64_t, ValueId> seen;
        for (auto& inst : block.instructions) {
            if (!is_pure(inst)) continue;
            if (inst.operands.empty()) continue;
            // Hash: opcode + operands.
            uint64_t h = static_cast<uint64_t>(inst.opcode);
            for (ValueId v : inst.operands) {
                h = h * 31 + v;
            }
            auto it = seen.find(h);
            if (it != seen.end()) {
                // Found a duplicate. Replace this instruction's result
                // with the existing one. We can't actually remove the
                // instruction here (it would invalidate block
                // iteration); DCE will clean it up after we rewrite
                // uses.
                // v0.3: just count it; a full CSE would rewrite all
                // uses of inst.result to it->second.
                (void)it;
                // For now, skip the rewrite — it requires a use-list
                // which we don't maintain yet.
            } else {
                seen[h] = inst.result;
            }
        }
    }
    return changes;
}

uint32_t Optimizer::dead_code_elimination(Function& fn) {
    uint32_t changes = 0;
    // Find all live values: start from terminators and side-effecting
    // instructions, mark their operands live, transitively.
    std::vector<bool> live(fn.next_value, false);
    // Mark params as live.
    for (ValueId p : fn.params) {
        if (p < live.size()) live[p] = true;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                // An instruction is live if it's impure OR its result
                // is live.
                bool inst_live = !is_pure(inst);
                if (inst.result != kInvalidValue && live[inst.result]) {
                    inst_live = true;
                }
                if (inst_live) {
                    for (ValueId v : inst.operands) {
                        if (v != kInvalidValue && v < live.size() && !live[v]) {
                            live[v] = true;
                            changed = true;
                        }
                    }
                    if (inst.mem_in != kInvalidValue && inst.mem_in < live.size() && !live[inst.mem_in]) {
                        live[inst.mem_in] = true;
                        changed = true;
                    }
                }
            }
        }
    }

    // Remove dead instructions.
    for (auto& block : fn.blocks) {
        auto it = std::remove_if(block.instructions.begin(),
                                  block.instructions.end(),
                                  [&](const Instruction& inst) {
            if (is_pure(inst) && (inst.result == kInvalidValue ||
                                  !live[inst.result])) {
                ++changes;
                return true;
            }
            return false;
        });
        block.instructions.erase(it, block.instructions.end());
    }
    return changes;
}

uint32_t Optimizer::remove_unreachable_blocks(Function& fn) {
    uint32_t changes = 0;
    if (fn.blocks.empty()) return 0;
    // Mark reachable blocks starting from entry.
    std::vector<bool> reachable(fn.blocks.size(), false);
    std::vector<BlockId> worklist = {fn.entry};
    while (!worklist.empty()) {
        BlockId b = worklist.back();
        worklist.pop_back();
        if (b >= reachable.size() || reachable[b]) continue;
        reachable[b] = true;
        for (BlockId s : fn.blocks[b].successors) {
            if (!reachable[s]) worklist.push_back(s);
        }
        // Also look at the terminator's targets.
        if (!fn.blocks[b].instructions.empty()) {
            const auto& term = fn.blocks[b].instructions.back();
            for (BlockId t : term.blocks) {
                if (t < reachable.size() && !reachable[t]) {
                    worklist.push_back(t);
                }
            }
        }
    }
    // Remove unreachable blocks.
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        if (!reachable[i] && !fn.blocks[i].instructions.empty()) {
            fn.blocks[i].instructions.clear();
            ++changes;
        }
    }
    return changes;
}

uint32_t Optimizer::simplify_cfg(Function& fn) {
    uint32_t changes = 0;
    // Merge blocks that have a single predecessor and that predecessor
    // ends with an unconditional Br to them.
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        auto& block = fn.blocks[i];
        if (block.predecessors.size() != 1) continue;
        BlockId pred = block.predecessors[0];
        if (pred >= fn.blocks.size() || pred == i) continue;
        auto& pred_block = fn.blocks[pred];
        if (pred_block.instructions.empty()) continue;
        auto& term = pred_block.instructions.back();
        if (term.opcode != Opcode::Br) continue;
        if (term.blocks.size() != 1 || term.blocks[0] != i) continue;
        // Merge: remove the Br from pred, move block's instructions to pred.
        pred_block.instructions.pop_back();
        for (auto& inst : block.instructions) {
            inst.block = pred;
            pred_block.instructions.push_back(std::move(inst));
        }
        pred_block.successors = block.successors;
        block.instructions.clear();
        block.predecessors.clear();
        ++changes;
    }
    return changes;
}

} // namespace tether::ssa
