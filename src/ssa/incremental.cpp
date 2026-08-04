// ssa/incremental.cpp — incremental compilation implementation

#include "ssa/incremental.hpp"

#include "ast/nodes.hpp"
#include "ssa/builder.hpp"
#include "ssa/optimizer.hpp"

namespace tether::ssa {

uint64_t IncrementalCompiler::hash_function(const ast::Item& fn) {
    // v0.3: hash the function name + body source range. A proper
    // implementation would hash the full AST + all referenced symbols.
    uint64_t h = 1469598103934665603ULL; // FNV offset
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(fn.name);
    mix(fn.range.start.file_id);
    mix(fn.range.start.offset);
    mix(fn.range.end.offset);
    // Mix in the number of params and body statements — a coarse
    // structural hash.
    mix(fn.params.size());
    if (fn.body) {
        mix(fn.body->stmts.size());
        mix(fn.body->trailing != nullptr);
    }
    return h;
}

bool IncrementalCompiler::compile_function(const ast::Item& fn_item,
                                           Module& mod) {
    uint64_t h = hash_function(fn_item);
    auto it = cache_.find(fn_item.name);
    if (it != cache_.end() && it->second.hash == h) {
        // Cache hit — reuse the cached SSA function.
        mod.functions.push_back(it->second.func);
        ++hits_;
        return false;
    }

    // Cache miss — lower the function to SSA.
    Builder builder(tc_, diag_, intern_, tc_.arena());
    builder.lower_module(ast::Module{});  // init
    // Lower just this function.
    // v0.3: the builder expects a Module; we create a temp module
    // and move the function into `mod`.
    Builder b2(tc_, diag_, intern_, tc_.arena());
    // Build a temp module containing just this function.
    ast::Module temp_mod;
    temp_mod.items.push_back(&fn_item);
    Module temp_ssa = b2.lower_module(temp_mod);
    if (!temp_ssa.functions.empty()) {
        Function& f = temp_ssa.functions[0];
        // Optimize.
        Optimizer opt(tc_, diag_);
        uint32_t changes = opt.run(temp_ssa);
        // Cache it.
        FunctionCache c;
        c.hash    = h;
        c.func    = std::move(f);
        c.changes = changes;
        cache_[fn_item.name] = c;
        // Move into the output module.
        mod.functions.push_back(std::move(cache_[fn_item.name].func));
        // Restore the cache entry (it was moved from).
        cache_[fn_item.name].hash    = h;
        cache_[fn_item.name].changes = changes;
    }
    ++misses_;
    return true;
}

} // namespace tether::ssa
