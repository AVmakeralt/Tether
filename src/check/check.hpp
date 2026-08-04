// check/check.hpp — type checker
//
// The type checker walks the resolved AST and verifies that every
// expression is well-typed. It depends on the resolver having already
// resolved all identifiers and types.
//
// v0.2 supports:
//   - Integer / float / bool / void primitive checking
//   - Reference types (ref T, mut ref T)
//   - Array, slice, tuple types
//   - Function types and call checking
//   - Struct field access
//   - Enum variant construction and matching
//   - Basic type inference for let-bindings without annotations
//   - Return-type checking
//   - Branch type agreement (if/else, match arms)
//
// v0.2 does NOT yet support:
//   - Generic monomorphization (generics are checked but not instantiated)
//   - Trait resolution
//   - Region / lifetime inference beyond the trivial case

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "resolve/resolve.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

namespace tether::check {

class TypeChecker {
public:
    TypeChecker(type::TypeContext& tc, DiagnosticEmitter& diag,
                resolve::Resolver& resolver, InternTable& intern)
        : tc_(tc), diag_(diag), resolver_(resolver), intern_(intern) {}

    // Check a module. Returns true on success.
    bool check_module(const ast::Module& m);

private:
    type::TypeContext&   tc_;
    DiagnosticEmitter&   diag_;
    resolve::Resolver&   resolver_;
    InternTable&         intern_;

    // The expected return type of the function currently being checked.
    type::TypePtr current_return_type_ = nullptr;
    // Whether we're inside a loop (for break/continue checking).
    bool in_loop_ = false;
    // Whether we're inside an unsafe block.
    bool in_unsafe_ = false;

    // Trait definitions: trait name → list of method signatures.
    struct TraitMethod {
        StrId    name;
        size_t   param_count;
        bool     has_return;
    };
    std::unordered_map<StrId, std::vector<TraitMethod>> traits_;

    // Check that an impl block satisfies its trait (if specified).
    void check_impl_satisfies_trait(const ast::Item& impl);

    void check_item(ast::ItemPtr item);
    void check_fn(ast::ItemPtr item);
    void check_struct(ast::ItemPtr item);
    void check_enum(ast::ItemPtr item);

    void check_block(ast::BlockPtr b, type::TypePtr expected);
    void check_stmt(ast::StmtPtr s);
    type::TypePtr check_expr(ast::ExprPtr e, type::TypePtr expected = nullptr);
    void check_pattern(ast::PatternPtr p, type::TypePtr expected);

    // Helper: check that `actual` is assignable to `expected`. Reports
    // an error at `loc` if not.
    bool expect_type(type::TypePtr expected, type::TypePtr actual,
                     tether::SourceRange loc, const char* context);
};

} // namespace tether::check
