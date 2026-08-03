// ssa/rewrite.cpp — rewrite rule implementation

#include "ssa/rewrite.hpp"

namespace tether::rewrite {

using namespace tether::ast;

void Rewriter::apply(ast::Module& mod,
                     const std::vector<ast::ItemPtr>& rules) {
    if (rules.empty()) return;
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 100) {
        changed = false;
        for (ItemPtr item : mod.items) {
            if (!item) continue;
            // Don't apply rewrites to the rewrite rules themselves.
            if (item->kind == ItemKind::Rewrite) continue;
            rewrite_item(item, rules);
        }
        ++iterations;
    }
}

bool Rewriter::match(ExprPtr pattern, ExprPtr expr, Bindings& bindings) {
    if (!pattern || !expr) return false;

    // A pattern that's an Ident with a name starting with a capital
    // letter is treated as a constructor name (e.g. Int, add). An
    // Ident with a lowercase letter is a pattern variable.
    //
    // For v0.5, we use a simpler rule: an Ident in pattern position
    // is always a pattern variable (binding). A Path is a constructor.
    // A Call in pattern position matches a Call with the same callee
    // and matching arguments.

    if (pattern->kind == ExprKind::Ident && pattern->path.size() == 1) {
        // Pattern variable — bind it.
        bindings[pattern->path[0]] = expr;
        return true;
    }

    if (pattern->kind != expr->kind) return false;

    switch (pattern->kind) {
        case ExprKind::IntLit:
            return pattern->int_value == expr->int_value;
        case ExprKind::BoolLit:
            return pattern->int_value == expr->int_value;
        case ExprKind::StringLit:
            return pattern->str_value == expr->str_value;
        case ExprKind::CharLit:
            return pattern->int_value == expr->int_value;
        case ExprKind::FloatLit:
            return pattern->float_value == expr->float_value;

        case ExprKind::Ident:
        case ExprKind::Path:
            // Same path?
            if (pattern->path.size() != expr->path.size()) return false;
            for (size_t i = 0; i < pattern->path.size(); ++i) {
                if (pattern->path[i] != expr->path[i]) return false;
            }
            return true;

        case ExprKind::Binary:
            if (pattern->binary_op != expr->binary_op) return false;
            return match(pattern->lhs, expr->lhs, bindings) &&
                   match(pattern->rhs, expr->rhs, bindings);

        case ExprKind::Unary:
            if (pattern->unary_op != expr->unary_op) return false;
            return match(pattern->lhs, expr->lhs, bindings);

        case ExprKind::Call: {
            // Match the callee.
            if (!match(pattern->lhs, expr->lhs, bindings)) return false;
            // Match arguments.
            if (pattern->args.size() != expr->args.size()) return false;
            for (size_t i = 0; i < pattern->args.size(); ++i) {
                if (!match(pattern->args[i], expr->args[i], bindings))
                    return false;
            }
            return true;
        }

        case ExprKind::FieldAccess:
            if (pattern->field_name != expr->field_name) return false;
            return match(pattern->lhs, expr->lhs, bindings);

        default:
            // For other expression kinds, require exact structural
            // equality (no pattern variables in subpositions).
            return false;
    }
}

ExprPtr Rewriter::instantiate(ExprPtr template_expr,
                              const Bindings& bindings) {
    if (!template_expr) return nullptr;

    // If it's a pattern variable, substitute the binding.
    if (template_expr->kind == ExprKind::Ident &&
        template_expr->path.size() == 1) {
        auto it = bindings.find(template_expr->path[0]);
        if (it != bindings.end()) {
            return it->second;
        }
        // Not a pattern variable — return as-is.
        return template_expr;
    }

    // For compound expressions, deep-copy with substitution.
    // v0.5: we don't deep-copy — we return the template as-is for
    // non-variable cases. A proper implementation would clone the
    // template expression tree. For now, this works for simple
    // replacements (the common case).
    switch (template_expr->kind) {
        case ExprKind::IntLit:
        case ExprKind::BoolLit:
        case ExprKind::StringLit:
        case ExprKind::CharLit:
        case ExprKind::FloatLit:
        case ExprKind::Ident:
        case ExprKind::Path:
            return template_expr;
        case ExprKind::Binary: {
            // Try to instantiate both sides. If either side is a
            // variable, substitute it.
            ExprPtr lhs = instantiate(template_expr->lhs, bindings);
            ExprPtr rhs = instantiate(template_expr->rhs, bindings);
            if (lhs != template_expr->lhs || rhs != template_expr->rhs) {
                // A substitution happened — create a new node.
                Expr e = *template_expr;
                e.lhs = lhs;
                e.rhs = rhs;
                return arena_.construct<Expr>(std::move(e));
            }
            return template_expr;
        }
        case ExprKind::Call: {
            bool changed = false;
            std::vector<ExprPtr> new_args;
            for (ExprPtr a : template_expr->args) {
                ExprPtr na = instantiate(a, bindings);
                if (na != a) changed = true;
                new_args.push_back(na);
            }
            ExprPtr callee = instantiate(template_expr->lhs, bindings);
            if (callee != template_expr->lhs) changed = true;
            if (changed) {
                Expr e = *template_expr;
                e.lhs = callee;
                e.args = std::move(new_args);
                return arena_.construct<Expr>(std::move(e));
            }
            return template_expr;
        }
        default:
            return template_expr;
    }
}

bool Rewriter::try_rewrite(ExprPtr& expr,
                           const std::vector<ast::ItemPtr>& rules) {
    for (ItemPtr rule : rules) {
        if (!rule) continue;
        for (const auto& arm : rule->rewrite_arms) {
            Bindings bindings;
            if (match(arm.pattern, expr, bindings)) {
                ExprPtr replacement = instantiate(arm.replacement, bindings);
                if (replacement && replacement != expr) {
                    expr = replacement;
                    return true;
                }
            }
        }
    }
    return false;
}

void Rewriter::rewrite_expr(ExprPtr& e,
                            const std::vector<ast::ItemPtr>& rules) {
    if (!e) return;

    // Recurse into children first (bottom-up rewriting). We need
    // const_cast because ExprPtr is const Expr* and the AST is
    // nominally immutable, but rewrite rules mutate it by design.
    if (e->lhs) {
        ExprPtr tmp = e->lhs;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->lhs) = tmp;
    }
    if (e->rhs) {
        ExprPtr tmp = e->rhs;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->rhs) = tmp;
    }
    for (auto& a : const_cast<std::vector<ExprPtr>&>(e->args)) {
        rewrite_expr(a, rules);
    }
    if (e->cond) {
        ExprPtr tmp = e->cond;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->cond) = tmp;
    }
    if (e->then_branch) {
        ExprPtr tmp = e->then_branch;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->then_branch) = tmp;
    }
    if (e->else_branch) {
        ExprPtr tmp = e->else_branch;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->else_branch) = tmp;
    }
    if (e->body) {
        ExprPtr tmp = e->body;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->body) = tmp;
    }
    if (e->index) {
        ExprPtr tmp = e->index;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->index) = tmp;
    }
    if (e->return_value) {
        ExprPtr tmp = e->return_value;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->return_value) = tmp;
    }
    if (e->alloc_value) {
        ExprPtr tmp = e->alloc_value;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(e->alloc_value) = tmp;
    }
    if (e->block) rewrite_block(e->block, rules);
    for (auto& arm : const_cast<std::vector<MatchArm>&>(e->arms)) {
        if (arm.body) {
            ExprPtr tmp = arm.body;
            rewrite_expr(tmp, rules);
            const_cast<ExprPtr&>(arm.body) = tmp;
        }
    }

    // Now try to rewrite this node.
    try_rewrite(e, rules);
}

void Rewriter::rewrite_block(ast::BlockPtr b,
                             const std::vector<ast::ItemPtr>& rules) {
    if (!b) return;
    for (auto& s : const_cast<std::vector<StmtPtr>&>(b->stmts)) {
        rewrite_stmt(s, rules);
    }
    if (b->trailing) {
        ExprPtr tmp = b->trailing;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(b->trailing) = tmp;
    }
}

void Rewriter::rewrite_stmt(StmtPtr s,
                            const std::vector<ast::ItemPtr>& rules) {
    if (!s) return;
    if (s->let_value) {
        ExprPtr tmp = s->let_value;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(s->let_value) = tmp;
    }
    if (s->expr) {
        ExprPtr tmp = s->expr;
        rewrite_expr(tmp, rules);
        const_cast<ExprPtr&>(s->expr) = tmp;
    }
    if (s->block) rewrite_block(s->block, rules);
}

void Rewriter::rewrite_item(ItemPtr item,
                            const std::vector<ast::ItemPtr>& rules) {
    if (!item) return;
    if (item->body) rewrite_block(item->body, rules);
    if (item->const_value)
        rewrite_expr(const_cast<ExprPtr&>(item->const_value), rules);
    for (ItemPtr m : item->impl_members) rewrite_item(m, rules);
    for (ItemPtr m : item->trait_members) rewrite_item(m, rules);
    if (item->inner) rewrite_item(item->inner, rules);
}

} // namespace tether::rewrite
