// borrow/borrow.hpp — borrow checker
//
// The borrow checker enforces Tether's ownership rules:
//
//   1. Every value has exactly one owner.
//   2. After `move x`, `x` is invalid.
//   3. Many shared borrows OR one mutable borrow — never both.
//   4. References do not outlive their referents.
//   5. Allocation domains (arenas) own every node allocated in them.
//
// The borrow checker is a flow-sensitive analysis: it walks the AST in
// execution order, tracking the state of every binding. At each point
// it knows:
//
//   - Whether a binding is initialized (has a value).
//   - Whether a binding is moved (its value has been moved out).
//   - How many shared borrows exist of a binding.
//   - Whether a mutable borrow exists of a binding.
//
// On `move x`, the binding is marked moved and any subsequent use is an
// error. On `borrow x`, a shared borrow is recorded. On `borrow mut x`,
// a mutable borrow is recorded and any other borrow is an error.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "resolve/resolve.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

#include <unordered_map>

namespace tether::borrow {

class BorrowChecker {
public:
    BorrowChecker(type::TypeContext& tc, DiagnosticEmitter& diag,
                  resolve::Resolver& resolver, InternTable& intern)
        : tc_(tc), diag_(diag), resolver_(resolver), intern_(intern) {}

    // Check a module. Returns true on success.
    bool check_module(const ast::Module& m);

private:
    type::TypeContext&   tc_;
    DiagnosticEmitter&   diag_;
    resolve::Resolver&   resolver_;
    InternTable&         intern_;

    // The state of a single binding at the current program point.
    struct BindingState {
        bool initialized = false;
        bool moved       = false;
        int  shared_borrows  = 0;
        bool mutable_borrow  = false;
    };

    // Keyed by the Decl's slot. We use the resolver's slot numbers so
    // that nested scopes with the same name don't collide.
    std::unordered_map<uint32_t, BindingState> bindings_;

    // ---- Helpers ----
    BindingState* get_state(uint32_t slot);
    void check_block(ast::BlockPtr b);
    void check_stmt(ast::StmtPtr s);
    void check_expr(ast::ExprPtr e);

    // Mark a binding as moved; report an error if it was already moved
    // or borrowed.
    void do_move(uint32_t slot, SourceRange loc);

    // Record a shared borrow; report an error if a mutable borrow
    // exists.
    void do_borrow_shared(uint32_t slot, SourceRange loc);

    // Record a mutable borrow; report an error if any borrow exists.
    void do_borrow_mut(uint32_t slot, SourceRange loc);

    // Check that a binding is usable (initialized, not moved).
    void check_usable(uint32_t slot, SourceRange loc);
};

} // namespace tether::borrow
