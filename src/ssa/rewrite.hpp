// ssa/rewrite.hpp — user-defined rewrite rules
//
// Rewrite rules are AST-to-AST transforms. The user writes:
//
//   rewrite ConstantFold {
//     add(Int(a), Int(b)) => Int(a + b),
//     mul(x, Int(1))      => x,
//     mul(x, Int(0))      => Int(0),
//   }
//
// The rewriter walks every expression in the module and tries to
// match each rule's pattern. If the pattern matches, the expression
// is replaced with the replacement (with pattern variables bound to
// the matched subexpressions).
//
// Rewrite rules run before SSA lowering. They operate on the AST,
// not on SSA — this keeps them scoped to the language's surface
// syntax, where the programmer can reason about them.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"

#include <unordered_map>
#include <vector>

namespace tether::rewrite {

class Rewriter {
public:
    Rewriter(DiagnosticEmitter& diag, InternTable& intern, Arena& arena)
        : diag_(diag), intern_(intern), arena_(arena) {}

    // Apply all rewrite rules in `rules` to every expression in `mod`.
    // Runs to a fixed point: if any rule fires, the whole pass runs
    // again, until no rule fires.
    void apply(ast::Module& mod, const std::vector<ast::ItemPtr>& rules);

private:
    DiagnosticEmitter&  diag_;
    InternTable&        intern_;
    Arena&              arena_;

    // Bindings: pattern variable name -> matched expression.
    using Bindings = std::unordered_map<StrId, ast::ExprPtr>;

    // Try to match `pattern` against `expr`. If successful, fills
    // `bindings` with the pattern variables and returns true.
    bool match(ast::ExprPtr pattern, ast::ExprPtr expr, Bindings& bindings);

    // Apply `bindings` to `template_expr`, producing a new expression
    // with pattern variables replaced by their bound values.
    ast::ExprPtr instantiate(ast::ExprPtr template_expr, const Bindings& bindings);

    // Try each rewrite rule on `expr`. If any fires, replaces `expr`
    // with the replacement and returns true.
    bool try_rewrite(ast::ExprPtr& expr,
                     const std::vector<ast::ItemPtr>& rules);

    // Walk an expression tree, applying rewrite rules at every node.
    void rewrite_expr(ast::ExprPtr& e,
                      const std::vector<ast::ItemPtr>& rules);
    void rewrite_block(ast::BlockPtr b,
                       const std::vector<ast::ItemPtr>& rules);
    void rewrite_stmt(ast::StmtPtr s,
                      const std::vector<ast::ItemPtr>& rules);
    void rewrite_item(ast::ItemPtr item,
                      const std::vector<ast::ItemPtr>& rules);
};

} // namespace tether::rewrite
