// ssa/partial_eval.hpp — partial evaluation / comptime interpreter
//
// The partial evaluator runs `comptime { ... }` blocks at compile time.
// It interprets SSA instructions on constant values and produces
// constant SSA values as output.
//
// v0.3 supports:
//   - Integer and boolean constant evaluation
//   - Arithmetic, bitwise, comparison operations
//   - Control flow (if/else, while) via constant-folding conditions
//   - Function calls to other comptime functions
//
// v0.3 does NOT yet support:
//   - Heap memory (alloc/load/store) — comptime values are pure
//   - Extern calls — those require linking against C at runtime
//   - Concurrency (spawn/await)

#pragma once

#include "ssa/node.hpp"
#include "diagnostics/diagnostics.hpp"
#include "types/types.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace tether::ssa {

class PartialEvaluator {
public:
    explicit PartialEvaluator(type::TypeContext& tc, DiagnosticEmitter& diag)
        : tc_(tc), diag_(diag) {}

    // Evaluate a function's body with the given constant arguments.
    // Returns the constant result, or std::nullopt if the function
    // can't be fully evaluated at compile time.
    std::optional<uint64_t> evaluate(const Function& fn,
                                      const std::vector<uint64_t>& args);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;

    // The function being evaluated.
    const Function* fn_ = nullptr;

    // Value table: SSA ValueId -> evaluated constant.
    std::unordered_map<ValueId, uint64_t> values_;

    // Execute a single block. Returns the next block to execute (or
    // kInvalidBlock if the function returned).
    BlockId execute_block(BlockId b);

    // Evaluate a single instruction. Returns true if the instruction
    // was successfully evaluated.
    bool eval_instruction(const Instruction& inst);

    // Look up a value. Returns 0 if unknown.
    uint64_t lookup(ValueId v) const {
        auto it = values_.find(v);
        return it != values_.end() ? it->second : 0;
    }
};

} // namespace tether::ssa
