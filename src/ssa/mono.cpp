// ssa/mono.cpp — generic monomorphization implementation

#include "ssa/mono.hpp"

#include <algorithm>
#include <sstream>

namespace tether::mono {

using namespace tether::ast;

ast::ModulePtr Monomorphizer::run(const ast::Module& m) {
    // Step 1: collect all instantiations needed.
    auto keys = collect_instantiations(m);

    // Step 2: instantiate each generic function.
    std::unordered_map<StrId, const ast::Item*> generic_fns;
    for (ItemPtr item : m.items) {
        if (item && item->kind == ItemKind::Fn && is_generic(*item)) {
            generic_fns[item->name] = item;
        }
    }

    for (const auto& key : keys) {
        auto it = generic_fns.find(key.function_name);
        if (it != generic_fns.end()) {
            (void)instantiate(*it->second, key.type_args);
        }
    }

    // Step 3: build the output module — original non-generic items
    // + instantiated functions, with call sites rewritten.
    auto out = arena_.construct<ast::Module>();
    out->range = m.range;
    out->module_path = m.module_path;

    for (ItemPtr item : m.items) {
        if (!item) continue;
        if (item->kind == ItemKind::Fn && is_generic(*item)) {
            // Skip the generic definition; only emit instantiations.
            continue;
        }
        out->items.push_back(item);
    }

    // Add instantiated functions.
    for (ItemPtr fn : instantiated_fns_) {
        out->items.push_back(fn);
    }

    // Rewrite call sites in all functions.
    for (ItemPtr item : out->items) {
        if (item && item->kind == ItemKind::Fn && item->body) {
            rewrite_block(const_cast<ast::Block&>(*item->body));
        }
    }

    return out;
}

std::vector<MonoKey> Monomorphizer::collect_instantiations(const ast::Module& m) {
    // v0.4: we don't yet parse explicit type arguments at call sites
    // (max<i32>(3, 4)). Instead, we infer the type arguments from
    // the argument types. For simplicity, we look for calls to generic
    // functions and record them with all-Int type arguments.
    //
    // A proper implementation would:
    //   1. Parse explicit type arguments (max<i32>(3, 4))
    //   2. Or infer from argument types during type checking
    //
    // For v0.4, we just collect calls to generic functions and
    // instantiate them with a single i64 type argument.
    std::vector<MonoKey> keys;

    for (ItemPtr item : m.items) {
        if (!item || item->kind != ItemKind::Fn || !item->body) continue;
        // Walk the body looking for calls to generic functions.
        // v0.4: simplified — just collect function names that are
        // called and check if they're generic.
        std::vector<StrId> called_names;
        auto collect_calls = [&](ast::ExprPtr e, auto& self) -> void {
            if (!e) return;
            if (e->kind == ExprKind::Call && e->lhs &&
                e->lhs->kind == ExprKind::Ident &&
                !e->lhs->path.empty()) {
                called_names.push_back(e->lhs->path[0]);
            }
            for (auto a : e->args) self(a, self);
            if (e->lhs) self(e->lhs, self);
            if (e->rhs) self(e->rhs, self);
            if (e->cond) self(e->cond, self);
            if (e->then_branch) self(e->then_branch, self);
            if (e->else_branch) self(e->else_branch, self);
            if (e->body) self(e->body, self);
        };
        for (auto s : item->body->stmts) {
            if (s && s->expr) collect_calls(s->expr, collect_calls);
            if (s && s->let_value) collect_calls(s->let_value, collect_calls);
        }
        if (item->body->trailing) {
            collect_calls(item->body->trailing, collect_calls);
        }

        // Check which called names are generic functions.
        for (ItemPtr fn_item : m.items) {
            if (!fn_item || fn_item->kind != ItemKind::Fn) continue;
            if (!is_generic(*fn_item)) continue;
            for (StrId called : called_names) {
                if (called == fn_item->name) {
                    MonoKey key;
                    key.function_name = fn_item->name;
                    // v0.4: instantiate with i64 for each type param.
                    for (size_t i = 0; i < fn_item->type_params.size(); ++i) {
                        key.type_args.push_back(tc_.i64());
                    }
                    // Check if we already have this instantiation.
                    if (instantiations_.find(key) == instantiations_.end()) {
                        keys.push_back(key);
                    }
                }
            }
        }
    }

    return keys;
}

StrId Monomorphizer::instantiate(const ast::Item& generic_fn,
                                  const std::vector<type::TypePtr>& type_args) {
    MonoKey key{generic_fn.name, type_args};
    auto it = instantiations_.find(key);
    if (it != instantiations_.end()) {
        return it->second;
    }

    // Create a copy of the function with type parameters substituted.
    std::string mangled = mangle(generic_fn.name, type_args);
    StrId new_name = intern_.intern(std::string_view(mangled));

    // Copy the AST node.
    auto new_fn = arena_.construct<ast::Item>(generic_fn);
    new_fn->name = new_name;
    new_fn->type_params.clear(); // instantiated fn has no type params

    // v0.4: we don't actually substitute types in the body — the SSA
    // builder will treat all integers as i64 anyway. A proper
    // implementation would walk the body and replace TypeVar references
    // with the concrete types.

    instantiations_[key] = new_name;
    instantiated_fns_.push_back(new_fn);
    return new_name;
}

std::string Monomorphizer::mangle(StrId name,
                                   const std::vector<type::TypePtr>& type_args) {
    std::string s = std::string(intern_.get(name));
    for (auto t : type_args) {
        s += "_";
        s += tc_.render(t);
    }
    return s;
}

void Monomorphizer::rewrite_block(ast::Block& b) {
    for (auto& s : b.stmts) {
        if (s) rewrite_stmt(*const_cast<ast::Stmt*>(s));
    }
    if (b.trailing) rewrite_expr(*const_cast<ast::Expr*>(b.trailing));
}

void Monomorphizer::rewrite_stmt(ast::Stmt& s) {
    if (s.let_value) rewrite_expr(*const_cast<ast::Expr*>(s.let_value));
    if (s.expr) rewrite_expr(*const_cast<ast::Expr*>(s.expr));
    if (s.block) rewrite_block(*const_cast<ast::Block*>(s.block));
}

void Monomorphizer::rewrite_expr(ast::Expr& e) {
    // Rewrite calls to generic functions.
    if (e.kind == ExprKind::Call && e.lhs &&
        e.lhs->kind == ExprKind::Ident && !e.lhs->path.empty()) {
        StrId callee = e.lhs->path[0];
        // Check if this is a generic function that was instantiated.
        for (const auto& [key, new_name] : instantiations_) {
            if (key.function_name == callee) {
                // Rewrite the call to use the instantiated name.
                const_cast<ast::Expr*>(e.lhs)->path[0] = new_name;
                break;
            }
        }
    }

    // Recurse into sub-expressions.
    if (e.lhs) rewrite_expr(*const_cast<ast::Expr*>(e.lhs));
    if (e.rhs) rewrite_expr(*const_cast<ast::Expr*>(e.rhs));
    for (auto& a : const_cast<std::vector<ast::ExprPtr>&>(e.args)) {
        if (a) rewrite_expr(*const_cast<ast::Expr*>(a));
    }
    if (e.cond) rewrite_expr(*const_cast<ast::Expr*>(e.cond));
    if (e.then_branch) rewrite_expr(*const_cast<ast::Expr*>(e.then_branch));
    if (e.else_branch) rewrite_expr(*const_cast<ast::Expr*>(e.else_branch));
    if (e.body) rewrite_expr(*const_cast<ast::Expr*>(e.body));
    if (e.index) rewrite_expr(*const_cast<ast::Expr*>(e.index));
    if (e.return_value) rewrite_expr(*const_cast<ast::Expr*>(e.return_value));
    if (e.alloc_value) rewrite_expr(*const_cast<ast::Expr*>(e.alloc_value));
    if (e.block) rewrite_block(*const_cast<ast::Block*>(e.block));
    for (auto& arm : const_cast<std::vector<ast::MatchArm>&>(e.arms)) {
        if (arm.body) rewrite_expr(*const_cast<ast::Expr*>(arm.body));
    }
}

} // namespace tether::mono
