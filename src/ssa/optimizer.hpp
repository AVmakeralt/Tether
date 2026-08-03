// ssa/optimizer.hpp — SSA optimization passes
//
// All passes are graph → graph transforms. They run to a fixed point:
// if a pass changes the module, it runs again, until no pass makes a
// change.
//
// v0.3 passes:
//   - constant_folding: fold ConstInt + ConstInt -> ConstInt
//   - dead_code_elimination: remove instructions whose result is unused
//   - common_subexpression_elimination: deduplicate identical instructions
//   - sparse_conditional_constant_propation (SCCP): propagate constants
//     through the CFG
//   - simplify_cfg: merge blocks with single predecessors, remove
//     unreachable blocks
//
// User-defined `rewrite` rules will run as an additional pass once the
// rewrite language is implemented.

#pragma once

#include "ssa/node.hpp"
#include "diagnostics/diagnostics.hpp"
#include "types/types.hpp"

namespace tether::ssa {

class Optimizer {
public:
    explicit Optimizer(type::TypeContext& tc, DiagnosticEmitter& diag)
        : tc_(tc), diag_(diag) {}

    // Run all passes to a fixed point. Returns the number of changes
    // made across all passes.
    uint32_t run(Module& mod);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;

    // Individual passes. Each returns the number of changes it made.
    uint32_t constant_folding(Function& fn);
    uint32_t dead_code_elimination(Function& fn);
    uint32_t common_subexpression_elimination(Function& fn);
    uint32_t simplify_cfg(Function& fn);
    uint32_t remove_unreachable_blocks(Function& fn);

    // Helper: check if an instruction is pure (side-effect-free and
    // safe to DCE if its result is unused).
    bool is_pure(const Instruction& inst) const;
};

} // namespace tether::ssa
