// ssa/mono.hpp — generic monomorphization
//
// Before SSA lowering, generic functions are instantiated for every
// concrete type they're called with. This pass walks the module,
// finds all calls to generic functions, and creates specialized
// copies with the type parameters replaced by concrete types.
//
// Example:
//   fn max<T: Ord>(a: T, b: T) -> T { ... }
//   fn f() { max<i32>(3, 4); max<i64>(5, 6); }
//
// After monomorphization:
//   fn max_i32(a: i32, b: i32) -> i32 { ... }
//   fn max_i64(a: i64, b: i64) -> i64 { ... }
//   fn f() { max_i32(3, 4); max_i64(5, 6); }
//
// LLVM has no concept of generic functions, so this must happen
// before SSA → LLVM lowering.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tether::mono {

// A monomorphization instance: (function name, type arguments).
struct MonoKey {
    StrId function_name;
    std::vector<type::TypePtr> type_args;

    bool operator==(const MonoKey& o) const {
        return function_name == o.function_name &&
               type_args == o.type_args;
    }
};

struct MonoKeyHash {
    size_t operator()(const MonoKey& k) const {
        size_t h = k.function_name;
        for (auto t : k.type_args) {
            h = h * 31 + reinterpret_cast<uintptr_t>(t);
        }
        return h;
    }
};

class Monomorphizer {
public:
    Monomorphizer(type::TypeContext& tc, DiagnosticEmitter& diag,
                  InternTable& intern, Arena& arena)
        : tc_(tc), diag_(diag), intern_(intern), arena_(arena) {}

    // Run monomorphization on a module. Returns a new module with
    // all generic functions instantiated and call sites rewritten.
    ast::ModulePtr run(const ast::Module& m);

private:
    type::TypeContext&  tc_;
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;
    Arena&              arena_;

    // Map from (generic fn name, type args) -> instantiated fn name.
    std::unordered_map<MonoKey, StrId, MonoKeyHash> instantiations_;

    // All instantiated functions, collected for the output module.
    std::vector<ast::ItemPtr> instantiated_fns_;

    // ---- Helpers ----

    // Find all calls to generic functions and record the type
    // arguments used. Returns the set of instantiation keys needed.
    std::vector<MonoKey> collect_instantiations(const ast::Module& m);

    // Instantiate a generic function with concrete type arguments.
    // Returns the new function's name.
    StrId instantiate(const ast::Item& generic_fn,
                      const std::vector<type::TypePtr>& type_args);

    // Generate a mangled name for an instantiation.
    std::string mangle(StrId name,
                       const std::vector<type::TypePtr>& type_args);

    // Walk an expression and rewrite calls to generic functions
    // to call the instantiated version instead.
    void rewrite_expr(ast::Expr& e);
    void rewrite_block(ast::Block& b);
    void rewrite_stmt(ast::Stmt& s);

    // Check if a function is generic (has type parameters).
    bool is_generic(const ast::Item& fn) const {
        return !fn.type_params.empty();
    }
};

} // namespace tether::mono
