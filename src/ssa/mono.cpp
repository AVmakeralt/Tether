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
    // Infer type arguments from call-site argument expressions.
    // For each call to a generic function, we determine the type of
    // each argument expression and substitute it for the corresponding
    // type parameter.
    //
    // Type inference rules (v0.6):
    //   - IntLit → i64 (default; could be refined by context)
    //   - FloatLit → f64
    //   - BoolLit → bool
    //   - StringLit → *const u8
    //   - CharLit → u32
    //   - Ident → look up the binding's type from the local scope
    //   - Call → the callee's return type
    //   - everything else → i64 (fallback)

    // Build a map of generic function name → type params.
    std::unordered_map<StrId, const ast::Item*> generic_fns;
    for (ItemPtr item : m.items) {
        if (item && item->kind == ItemKind::Fn && is_generic(*item)) {
            generic_fns[item->name] = item;
        }
    }

    // Build a map of function name → return type (for Call inference).
    // v0.6: we only resolve primitive return types.
    auto infer_expr_type = [&](ast::ExprPtr e,
                               const std::unordered_map<StrId, type::TypePtr>& locals)
        -> type::TypePtr {
        if (!e) return tc_.i64();
        switch (e->kind) {
            case ExprKind::IntLit:    return tc_.i64();
            case ExprKind::FloatLit:  return tc_.f64();
            case ExprKind::BoolLit:   return tc_.boolean();
            case ExprKind::StringLit: return tc_.make_raw_ptr(tc_.u8(), false);
            case ExprKind::CharLit:   return tc_.u32();
            case ExprKind::Ident:
                if (!e->path.empty()) {
                    auto it = locals.find(e->path[0]);
                    if (it != locals.end()) return it->second;
                }
                return tc_.i64();
            case ExprKind::Call: {
                // Look up the callee's return type.
                if (e->lhs && e->lhs->kind == ExprKind::Ident &&
                    !e->lhs->path.empty()) {
                    for (ItemPtr item : m.items) {
                        if (item && item->kind == ItemKind::Fn &&
                            item->name == e->lhs->path[0] &&
                            item->return_type) {
                            // Resolve the return type.
                            if (!item->return_type->path.empty()) {
                                auto t = tc_.lookup_primitive(
                                    intern_.get(item->return_type->path[0]));
                                if (t) return t;
                            }
                        }
                    }
                }
                return tc_.i64();
            }
            default:
                return tc_.i64();
        }
    };

    std::vector<MonoKey> keys;

    for (ItemPtr item : m.items) {
        if (!item || item->kind != ItemKind::Fn || !item->body) continue;

        // Build a local scope for this function: param name → type.
        std::unordered_map<StrId, type::TypePtr> locals;
        for (const auto& p : item->params) {
            if (p.name != kInvalidStrId && p.type && !p.type->path.empty()) {
                auto t = tc_.lookup_primitive(intern_.get(p.type->path[0]));
                if (t) locals[p.name] = t;
            }
        }
        // Also collect let-bindings as we walk (simple scan).
        for (auto s : item->body->stmts) {
            if (s && s->kind == ast::StmtKind::Let &&
                s->let_name != kInvalidStrId && s->let_type &&
                !s->let_type->path.empty()) {
                auto t = tc_.lookup_primitive(intern_.get(s->let_type->path[0]));
                if (t) locals[s->let_name] = t;
            }
        }

        // Walk the body looking for calls to generic functions.
        auto collect_calls = [&](ast::ExprPtr e, auto& self) -> void {
            if (!e) return;
            if (e->kind == ExprKind::Call && e->lhs &&
                e->lhs->kind == ExprKind::Ident &&
                !e->lhs->path.empty()) {
                StrId callee = e->lhs->path[0];
                auto git = generic_fns.find(callee);
                if (git != generic_fns.end()) {
                    const ast::Item& gfn = *git->second;
                    // Infer type args from the argument expressions.
                    MonoKey key;
                    key.function_name = callee;
                    // For each type parameter, find the first argument
                    // whose position maps to it and infer the type.
                    // v0.6: we assume type params appear in order in
                    // the parameter list. So type param T_i corresponds
                    // to parameter i, and we infer from argument i.
                    for (size_t i = 0; i < gfn.type_params.size(); ++i) {
                        if (i < e->args.size()) {
                            key.type_args.push_back(
                                infer_expr_type(e->args[i], locals));
                        } else {
                            key.type_args.push_back(tc_.i64());
                        }
                    }
                    if (instantiations_.find(key) == instantiations_.end()) {
                        keys.push_back(key);
                    }
                }
            }
            for (auto a : e->args) self(a, self);
            if (e->lhs) self(e->lhs, self);
            if (e->rhs) self(e->rhs, self);
            if (e->cond) self(e->cond, self);
            if (e->then_branch) self(e->then_branch, self);
            if (e->else_branch) self(e->else_branch, self);
            if (e->body) self(e->body, self);
            if (e->block) {
                for (auto s : e->block->stmts) {
                    if (s && s->expr) self(s->expr, self);
                    if (s && s->let_value) self(s->let_value, self);
                }
                if (e->block->trailing) self(e->block->trailing, self);
            }
        };
        for (auto s : item->body->stmts) {
            if (s && s->expr) collect_calls(s->expr, collect_calls);
            if (s && s->let_value) collect_calls(s->let_value, collect_calls);
        }
        if (item->body->trailing) {
            collect_calls(item->body->trailing, collect_calls);
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

    // Build a substitution map: type param name → concrete type.
    // The AST stores type params by name (StrId), and references to
    // them in param/return types are ast::TypeKind::Named with
    // path[0] == the type param's name. We walk the new fn's
    // signature and replace those references with concrete types.
    //
    // v0.9: this replaces the v0.4 stub that relied on the SSA
    // builder's i64 widening. With real integer widths, the
    // signature must use the actual instantiated types — otherwise
    // a `fn id<T>(x: T) -> T` instantiated at i32 would still
    // declare itself as taking `%struct.T`, which is broken.
    std::unordered_map<StrId, type::TypePtr> subst;
    for (size_t i = 0; i < generic_fn.type_params.size() &&
                        i < type_args.size(); ++i) {
        subst[generic_fn.type_params[i].name] = type_args[i];
    }

    // Helper: substitute one AST type. Returns a new (arena-allocated)
    // ast::Type if a substitution was made; returns the original
    // pointer if no change was needed.
    auto subst_type = [&](ast::TypePtr t) -> ast::TypePtr {
        if (!t) return t;
        if (t->kind == ast::TypeKind::Named && !t->path.empty()) {
            auto sit = subst.find(t->path[0]);
            if (sit != subst.end()) {
                // Render the concrete type back to a primitive name
                // and rebuild the AST type. We only handle primitives
                // here (integers, bool, etc.) since generics over
                // user-defined types would require more work.
                std::string name = tc_.render(sit->second);
                ast::Type nt;
                nt.kind = ast::TypeKind::Named;
                nt.range = t->range;
                nt.path = {intern_.intern(std::string_view(name))};
                return arena_.construct<ast::Type>(std::move(nt));
            }
        }
        // For composite types (Ref, RawPtr, etc.), recurse into base.
        // v0.9 keeps this shallow — only the top-level Named type is
        // substituted. Deeper substitution (e.g. ref T → ref i32) is
        // left for v1.0.
        return t;
    };

    // Substitute param types.
    auto& params = const_cast<std::vector<ast::Param>&>(new_fn->params);
    for (auto& p : params) {
        if (p.type) {
            p.type = subst_type(p.type);
        }
    }
    // Substitute return type.
    if (new_fn->return_type) {
        const_cast<ast::Item*>(new_fn)->return_type =
            subst_type(new_fn->return_type);
    }

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
        // Re-infer the type args at this call site to find the
        // correct instantiation.
        // v0.6: we don't have the local scope here, so we try each
        // instantiation and pick the first one whose type args are
        // compatible with the argument expressions.
        // A proper implementation would pass the scope through.
        //
        // For now: if there's only one instantiation of this function,
        // use it. If there are multiple, we need to infer — but we
        // can't without the scope. Default to the first.
        std::vector<StrId> candidates;
        for (const auto& [key, new_name] : instantiations_) {
            if (key.function_name == callee) {
                candidates.push_back(new_name);
            }
        }
        if (!candidates.empty()) {
            // Infer argument types at this call site.
            // We do a simple inference: IntLit→i64, BoolLit→bool,
            // FloatLit→f64, else→i64.
            std::vector<type::TypePtr> arg_types;
            for (auto a : e.args) {
                if (!a) { arg_types.push_back(tc_.i64()); continue; }
                switch (a->kind) {
                    case ExprKind::IntLit:    arg_types.push_back(tc_.i64()); break;
                    case ExprKind::BoolLit:   arg_types.push_back(tc_.boolean()); break;
                    case ExprKind::FloatLit:  arg_types.push_back(tc_.f64()); break;
                    case ExprKind::CharLit:   arg_types.push_back(tc_.u32()); break;
                    case ExprKind::StringLit: arg_types.push_back(tc_.make_raw_ptr(tc_.u8(), false)); break;
                    default:                  arg_types.push_back(tc_.i64()); break;
                }
            }
            // Find the instantiation whose type args match.
            StrId best = candidates[0];
            for (const auto& [key, new_name] : instantiations_) {
                if (key.function_name != callee) continue;
                if (key.type_args.size() != arg_types.size()) continue;
                bool match = true;
                for (size_t i = 0; i < key.type_args.size(); ++i) {
                    if (key.type_args[i] != arg_types[i]) { match = false; break; }
                }
                if (match) { best = new_name; break; }
            }
            const_cast<ast::Expr*>(e.lhs)->path[0] = best;
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
