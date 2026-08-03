// ssa/incremental.hpp — incremental compilation cache
//
// The incremental compiler caches SSA per function. On recompilation,
// only functions whose dependencies have changed are re-lowered and
// re-optimized. LLVM's ThinLTO is whole-module; it can't do per-
// function incremental, so Tether does it at the SSA layer.
//
// v0.3 uses a simple hash-based dependency scheme:
//
//   function_hash(fn) = hash(fn's AST) ^ hash(fn's imported symbols)
//
// If the hash matches the cached hash, the function is reused.
// Otherwise, it's re-lowered from the AST and re-optimized.

#pragma once

#include "ast/nodes.hpp"
#include "ssa/node.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <string>
#include <unordered_map>

namespace tether::ssa {

struct FunctionCache {
    uint64_t  hash    = 0;       // dependency hash
    Function  func;              // cached SSA function
    uint32_t  changes = 0;       // optimizer changes from last run
};

class IncrementalCompiler {
public:
    explicit IncrementalCompiler(type::TypeContext& tc,
                                 DiagnosticEmitter& diag,
                                 InternTable& intern)
        : tc_(tc), diag_(diag), intern_(intern) {}

    // Lower and optimize a function. Returns true if the function
    // was newly compiled (cache miss); false if it was reused.
    bool compile_function(const ast::Item& fn_item, Module& mod);

    // Compute a dependency hash for a function. Two functions with
    // the same hash are considered equivalent and the cached version
    // is reused.
    static uint64_t hash_function(const ast::Item& fn);

    // Statistics.
    uint32_t cache_hits()  const { return hits_; }
    uint32_t cache_misses() const { return misses_; }

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;

    // Cache: function name -> cached SSA function.
    std::unordered_map<StrId, FunctionCache> cache_;

    uint32_t hits_   = 0;
    uint32_t misses_ = 0;
};

} // namespace tether::ssa
