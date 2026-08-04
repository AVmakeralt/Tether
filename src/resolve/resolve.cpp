// resolve/resolve.cpp — name resolution implementation

#include "resolve/resolve.hpp"

#include <utility>

namespace tether::resolve {

using namespace tether::ast;
using namespace tether::type;

bool Resolver::resolve_module(const ast::Module& m) {
    // First pass: register all top-level declarations so they're
    // visible to each other regardless of declaration order. This
    // matches the spec: "Simple hierarchy. Visibility is controlled
    // by 'export' — everything else is private to the module."
    for (ItemPtr item : m.items) {
        if (!item) continue;
        switch (item->kind) {
            case ItemKind::Module:
            case ItemKind::Import:
                // Handled below.
                break;
            case ItemKind::Fn: {
                Decl d;
                d.kind   = Decl::Kind::Function;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Struct: {
                Decl d;
                d.kind   = Decl::Kind::Struct;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Enum: {
                Decl d;
                d.kind   = Decl::Kind::Enum;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Union: {
                Decl d;
                d.kind   = Decl::Kind::Union;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Trait: {
                Decl d;
                d.kind   = Decl::Kind::Trait;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::TypeAlias: {
                Decl d;
                d.kind   = Decl::Kind::TypeAlias;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Const: {
                Decl d;
                d.kind   = Decl::Kind::Const;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Static: {
                Decl d;
                d.kind   = Decl::Kind::Static;
                d.name   = item->name;
                d.item   = item;
                d.range  = item->range;
                register_decl(std::move(d));
                break;
            }
            case ItemKind::Extern: {
                if (item->extern_decl) {
                    Decl d;
                    d.kind   = Decl::Kind::ExternFn;
                    d.name   = item->extern_decl->name;
                    d.item   = item->extern_decl;
                    d.range  = item->range;
                    register_decl(std::move(d));
                }
                break;
            }
            case ItemKind::Export:
                // Export wraps an inner item; the inner item is
                // registered by its own case when resolve_item walks
                // it below.
                break;
        }
    }

    // Second pass: resolve bodies / signatures.
    // Process imports: register the last component of each import path
    // as a name in the current scope. This allows `import foo::bar`
    // to be used as `bar::function()`.
    for (ItemPtr item : m.items) {
        if (!item || item->kind != ItemKind::Import) continue;
        if (item->path.empty()) continue;
        StrId last = item->path.back();
        // Register a placeholder decl for the imported module.
        // The actual resolution happens when we see `last::name` —
        // we treat it as a path expression.
        if (top_level_.find(last) == top_level_.end()) {
            Decl d;
            d.kind  = Decl::Kind::Module;
            d.name  = last;
            d.range = item->range;
            register_decl(std::move(d));
        }
    }

    for (ItemPtr item : m.items) {
        if (!item) continue;
        resolve_item(item);
    }
    return !diag_.has_errors();
}

Decl* Resolver::register_decl(Decl d) {
    Decl* ptr = arena_.construct<Decl>(std::move(d));
    top_level_[ptr->name] = ptr;
    return ptr;
}

DeclPtr Resolver::lookup_local(StrId name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->names.find(name);
        if (found != it->names.end()) return found->second;
    }
    auto found = top_level_.find(name);
    if (found != top_level_.end()) return found->second;
    return nullptr;
}

TypePtr Resolver::resolve_type(ast::TypePtr t) {
    if (!t) return tc_.make_error();
    switch (t->kind) {
        case ast::TypeKind::Named: {
            if (t->path.empty()) return tc_.make_error();
            // First component is the name we look up.
            StrId first = t->path[0];
            std::string_view first_text = intern_.get(first);

            // Try primitive first.
            TypePtr prim = tc_.lookup_primitive(first_text);
            if (prim) return prim;

            // Try top-level type decl.
            auto it = top_level_.find(first);
            if (it == top_level_.end()) {
                diag_.error(t->range,
                    std::string("unknown type '") +
                    std::string(first_text) + "'");
                return tc_.make_error();
            }
            DeclPtr d = it->second;
            // Resolve generic arguments.
            std::vector<TypePtr> args;
            args.reserve(t->args.size());
            for (ast::TypePtr a : t->args) {
                args.push_back(resolve_type(a));
            }
            switch (d->kind) {
                case Decl::Kind::Struct: return tc_.make_struct(first, std::move(args));
                case Decl::Kind::Enum:   return tc_.make_enum(first, std::move(args));
                case Decl::Kind::Union:  return tc_.make_union(first, std::move(args));
                case Decl::Kind::Trait:  return tc_.make_trait(first);
                case Decl::Kind::TypeAlias:
                    // v0.2: type aliases are not yet resolved; treat
                    // as the alias's underlying type.
                    return d->type ? d->type : tc_.make_error();
                default:
                    diag_.error(t->range,
                        std::string("'") + std::string(first_text) +
                        "' is not a type");
                    return tc_.make_error();
            }
        }
        case ast::TypeKind::Ref: {
            TypePtr base = resolve_type(t->base);
            return tc_.make_ref(base, t->is_mut, t->region);
        }
        case ast::TypeKind::BorrowRef: {
            TypePtr base = resolve_type(t->base);
            return tc_.make_ref(base, t->is_mut, t->region);
        }
        case ast::TypeKind::RawPtr: {
            TypePtr base = resolve_type(t->base);
            return tc_.make_raw_ptr(base, t->is_mut);
        }
        case ast::TypeKind::Array: {
            TypePtr elem = resolve_type(t->base);
            // Array length must be a constant integer literal.
            // v0.2 only supports literal lengths.
            uint64_t size = 0;
            if (t->length && t->length->kind == ast::ExprKind::IntLit) {
                size = t->length->int_value;
            } else if (t->length) {
                diag_.error(t->length->range,
                            "array length must be an integer literal");
            }
            return tc_.make_array(elem, size);
        }
        case ast::TypeKind::Slice: {
            TypePtr elem = resolve_type(t->base);
            return tc_.make_slice(elem);
        }
        case ast::TypeKind::Tuple: {
            std::vector<TypePtr> args;
            for (ast::TypePtr a : t->args) args.push_back(resolve_type(a));
            return tc_.make_tuple(std::move(args));
        }
        case ast::TypeKind::Fn: {
            std::vector<TypePtr> params;
            for (ast::TypePtr a : t->args) params.push_back(resolve_type(a));
            TypePtr ret = t->return_type ? resolve_type(t->return_type)
                                         : tc_.void_type();
            return tc_.make_fn(std::move(params), ret);
        }
        case ast::TypeKind::Infer:
            // Infer types become inference variables during checking.
            return tc_.make_error();
    }
    return tc_.make_error();
}

void Resolver::resolve_item(ItemPtr item) {
    if (!item) return;
    switch (item->kind) {
        case ItemKind::Fn:        resolve_fn(item); break;
        case ItemKind::Struct:    resolve_struct(item); break;
        case ItemKind::Enum:      resolve_enum(item); break;
        case ItemKind::Union:     resolve_union(item); break;
        case ItemKind::Trait:     resolve_trait(item); break;
        case ItemKind::Impl:      resolve_impl(item); break;
        case ItemKind::TypeAlias: resolve_type_alias(item); break;
        case ItemKind::Const:     resolve_const(item, false); break;
        case ItemKind::Static:    resolve_const(item, true); break;
        case ItemKind::Extern:    resolve_extern(item); break;
        case ItemKind::Export:
            resolve_item(item->inner);
            break;
        case ItemKind::Module:
        case ItemKind::Import:
        case ItemKind::Rewrite:
        case ItemKind::Macro:
            // Rewrites and macros are handled by the rewriter pass,
            // not by name resolution.
            break;
    }
}

void Resolver::resolve_fn(ItemPtr item) {
    if (!item) return;
    // Resolve parameter types and return type.
    std::vector<TypePtr> param_types;
    param_types.reserve(item->params.size());
    for (const auto& p : item->params) {
        TypePtr t = resolve_type(p.type);
        param_types.push_back(t);
    }
    TypePtr ret = item->return_type ? resolve_type(item->return_type)
                                    : tc_.void_type();
    TypePtr fn_type = tc_.make_fn(std::move(param_types), ret);
    // Update the top-level decl with the resolved type.
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = fn_type;
    }

    if (item->body) {
        push_scope();
        // Register parameters in the function's scope.
        for (size_t i = 0; i < item->params.size(); ++i) {
            const auto& p = item->params[i];
            Decl d;
            d.kind  = p.is_self ? Decl::Kind::Self : Decl::Kind::Param;
            d.name  = p.name;
            d.type  = i < param_types.size() ? param_types[i] : tc_.make_error();
            d.range = p.range;
            d.slot  = next_slot_++;
            Decl* ptr = arena_.construct<Decl>(std::move(d));
            if (p.name != kInvalidStrId) {
                scopes_.back().names[p.name] = ptr;
            }
        }
        resolve_block(item->body);
        pop_scope();
    }
}

void Resolver::resolve_struct(ItemPtr item) {
    // Build a tuple type from the fields.
    std::vector<TypePtr> field_types;
    field_types.reserve(item->fields.size());
    for (const auto& f : item->fields) {
        field_types.push_back(resolve_type(f.type));
    }
    // Record the struct type on the decl.
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type =
            tc_.make_struct(item->name);
    }
    // Register fields for lookup by the type checker.
    for (uint32_t i = 0; i < item->fields.size(); ++i) {
        Decl d;
        d.kind      = Decl::Kind::Field;
        d.name      = item->fields[i].name;
        d.type      = field_types[i];
        d.range     = item->fields[i].range;
        d.field_idx = i;
        d.item      = item;
        // Note: fields are NOT registered in top_level_ — they're
        // looked up via the struct's Decl. v0.2 keeps them scoped to
        // the struct; the type checker will resolve field accesses.
        (void)d;
    }
}

void Resolver::resolve_enum(ItemPtr item) {
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = tc_.make_enum(item->name);
    }
    // Register variants.
    for (uint32_t i = 0; i < item->variants.size(); ++i) {
        const auto& v = item->variants[i];
        std::vector<TypePtr> arg_types;
        for (ast::TypePtr a : v.args) arg_types.push_back(resolve_type(a));
        Decl d;
        d.kind        = Decl::Kind::Variant;
        d.name        = v.name;
        d.parent_enum = item;
        d.variant_idx = i;
        d.range       = v.range;
        Decl* ptr = arena_.construct<Decl>(std::move(d));
        top_level_[v.name] = ptr;
    }
}

void Resolver::resolve_union(ItemPtr item) {
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = tc_.make_union(item->name);
    }
}

void Resolver::resolve_trait(ItemPtr item) {
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = tc_.make_trait(item->name);
    }
    // Trait members are signatures; resolve them so that impls can
    // be checked against them.
    for (ItemPtr m : item->trait_members) {
        if (m) resolve_fn(m);
    }
}

void Resolver::resolve_impl(ItemPtr item) {
    // Resolve the impl type and (if present) the trait.
    (void)resolve_type(item->impl_type);
    if (item->impl_trait) (void)resolve_type(item->impl_trait);
    // Resolve each member function.
    for (ItemPtr m : item->impl_members) {
        if (m) resolve_fn(m);
    }
}

void Resolver::resolve_type_alias(ItemPtr item) {
    TypePtr t = resolve_type(item->alias_type);
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = t;
    }
}

void Resolver::resolve_const(ItemPtr item, bool is_static) {
    (void)is_static;
    TypePtr t = item->const_type ? resolve_type(item->const_type)
                                 : tc_.make_error();
    auto it = top_level_.find(item->name);
    if (it != top_level_.end()) {
        it->second->type = t;
    }
    push_scope();
    if (item->const_value) (void)resolve_expr(item->const_value);
    pop_scope();
}

void Resolver::resolve_extern(ItemPtr item) {
    if (!item->extern_decl) return;
    ItemPtr fn = item->extern_decl;
    std::vector<TypePtr> param_types;
    for (const auto& p : fn->params) {
        param_types.push_back(resolve_type(p.type));
    }
    TypePtr ret = fn->return_type ? resolve_type(fn->return_type)
                                  : tc_.void_type();
    TypePtr fn_type = tc_.make_fn(std::move(param_types), ret);
    auto it = top_level_.find(fn->name);
    if (it != top_level_.end()) {
        it->second->type = fn_type;
    }
}

void Resolver::resolve_block(ast::BlockPtr b) {
    if (!b) return;
    push_scope();
    for (StmtPtr s : b->stmts) {
        if (s) resolve_stmt(s);
    }
    if (b->trailing) (void)resolve_expr(b->trailing);
    pop_scope();
}

void Resolver::resolve_stmt(StmtPtr s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Let: {
            TypePtr t = s->let_type ? resolve_type(s->let_type)
                                     : tc_.make_error();
            if (s->let_value) {
                TypePtr vt = resolve_expr(s->let_value);
                (void)vt;
            }
            Decl d;
            d.kind  = Decl::Kind::Binding;
            d.name  = s->let_name;
            d.type  = t;
            d.range = s->range;
            d.slot  = next_slot_++;
            Decl* ptr = arena_.construct<Decl>(std::move(d));
            scopes_.back().names[s->let_name] = ptr;
            break;
        }
        case StmtKind::Expr:
            (void)resolve_expr(s->expr);
            break;
        case StmtKind::Return:
            if (s->expr) (void)resolve_expr(s->expr);
            break;
        case StmtKind::Defer:
            if (s->expr) (void)resolve_expr(s->expr);
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        case StmtKind::Unsafe:
        case StmtKind::Block:
            resolve_block(s->block);
            break;
    }
}

TypePtr Resolver::resolve_expr(ExprPtr e) {
    if (!e) return tc_.make_error();
    switch (e->kind) {
        case ExprKind::IntLit:
            // Default to i32 for integer literals; the type checker
            // will refine based on context.
            return tc_.i32();
        case ExprKind::FloatLit:
            return tc_.f64();
        case ExprKind::StringLit:
            // Strings lower to *const u8 in v0.2.
            return tc_.make_raw_ptr(tc_.u8(), false);
        case ExprKind::CharLit:
            return tc_.u32();
        case ExprKind::BoolLit:
            return tc_.boolean();
        case ExprKind::Ident: {
            if (e->path.empty()) return tc_.make_error();
            DeclPtr d = lookup_local(e->path[0]);
            if (!d) {
                diag_.error(e->range,
                    std::string("unknown identifier '") +
                    std::string(intern_.get(e->path[0])) + "'");
                return tc_.make_error();
            }
            return d->type ? d->type : tc_.make_error();
        }
        case ExprKind::Path: {
            if (e->path.empty()) return tc_.make_error();
            DeclPtr d = lookup_local(e->path[0]);
            if (!d) {
                diag_.error(e->range,
                    std::string("unknown path '") +
                    std::string(intern_.get(e->path[0])) + "'");
                return tc_.make_error();
            }
            return d->type ? d->type : tc_.make_error();
        }
        case ExprKind::Unary: {
            TypePtr base = resolve_expr(e->lhs);
            switch (e->unary_op) {
                case UnaryOp::Neg:
                case UnaryOp::BitNot:
                    return base;
                case UnaryOp::Not:
                    return tc_.boolean();
                case UnaryOp::Deref:
                    if (base && base->kind == type::Kind::Ref) {
                        return base->base;
                    }
                    if (base && base->kind == type::Kind::RawPtr) {
                        return base->base;
                    }
                    return tc_.make_error();
                case UnaryOp::Borrow:
                    return tc_.make_ref(base, false);
                case UnaryOp::BorrowMut:
                    return tc_.make_ref(base, true);
                case UnaryOp::Move:
                    return base;
            }
            return tc_.make_error();
        }
        case ExprKind::Binary: {
            TypePtr lt = resolve_expr(e->lhs);
            TypePtr rt = resolve_expr(e->rhs);
            (void)lt; (void)rt;
            switch (e->binary_op) {
                case BinaryOp::Eq: case BinaryOp::Neq:
                case BinaryOp::Lt:  case BinaryOp::Gt:
                case BinaryOp::Le:  case BinaryOp::Ge:
                case BinaryOp::And: case BinaryOp::Or:
                    return tc_.boolean();
                default:
                    return tc_.i32();
            }
        }
        case ExprKind::Assign: {
            (void)resolve_expr(e->lhs);
            (void)resolve_expr(e->rhs);
            return tc_.void_type();
        }
        case ExprKind::Call: {
            TypePtr callee = resolve_expr(e->lhs);
            for (ExprPtr a : e->args) (void)resolve_expr(a);
            if (callee && callee->kind == type::Kind::Fn) {
                return callee->return_type ? callee->return_type
                                           : tc_.void_type();
            }
            return tc_.make_error();
        }
        case ExprKind::MethodCall: {
            (void)resolve_expr(e->lhs);
            for (ExprPtr a : e->args) (void)resolve_expr(a);
            return tc_.make_error();
        }
        case ExprKind::FieldAccess: {
            (void)resolve_expr(e->lhs);
            return tc_.make_error();
        }
        case ExprKind::Index: {
            TypePtr base = resolve_expr(e->lhs);
            (void)resolve_expr(e->index);
            if (base && base->kind == type::Kind::Array) return base->base;
            if (base && base->kind == type::Kind::Slice) return base->base;
            return tc_.make_error();
        }
        case ExprKind::Question: {
            // x? unwraps Result<T,E> to T, propagating E. v0.2 just
            // returns the inner type if known.
            return resolve_expr(e->lhs);
        }
        case ExprKind::Block:
            resolve_block(e->block);
            if (e->block && e->block->trailing) {
                return resolve_expr(e->block->trailing);
            }
            return tc_.void_type();
        case ExprKind::If: {
            (void)resolve_expr(e->cond);
            TypePtr then_t = resolve_expr(e->then_branch);
            if (e->else_branch) {
                TypePtr else_t = resolve_expr(e->else_branch);
                return then_t ? then_t : else_t;
            }
            return tc_.void_type();
        }
        case ExprKind::Match: {
            (void)resolve_expr(e->cond);
            TypePtr result = tc_.make_error();
            for (const auto& arm : e->arms) {
                resolve_pattern(arm.pattern);
                TypePtr t = resolve_expr(arm.body);
                if (!tc_.is_error(result)) result = t;
            }
            return result;
        }
        case ExprKind::Loop: {
            (void)resolve_expr(e->body);
            return tc_.void_type();
        }
        case ExprKind::While: {
            (void)resolve_expr(e->cond);
            (void)resolve_expr(e->body);
            return tc_.void_type();
        }
        case ExprKind::For: {
            (void)resolve_expr(e->iterable);
            push_scope();
            Decl d;
            d.kind  = Decl::Kind::Binding;
            d.name  = e->loop_var;
            d.type  = tc_.make_error();
            d.slot  = next_slot_++;
            Decl* ptr = arena_.construct<Decl>(std::move(d));
            scopes_.back().names[e->loop_var] = ptr;
            (void)resolve_expr(e->body);
            pop_scope();
            return tc_.void_type();
        }
        case ExprKind::Return: {
            if (e->return_value) (void)resolve_expr(e->return_value);
            return tc_.void_type();
        }
        case ExprKind::Break:
        case ExprKind::Continue:
            return tc_.void_type();
        case ExprKind::Defer:
            (void)resolve_expr(e->lhs);
            return tc_.void_type();
        case ExprKind::Alloc: {
            if (e->alloc_value) (void)resolve_expr(e->alloc_value);
            return tc_.make_error();
        }
        case ExprKind::Move:
            return resolve_expr(e->lhs);
        case ExprKind::Borrow:
            return tc_.make_ref(resolve_expr(e->lhs),
                                e->int_value != 0);
        case ExprKind::Unsafe:
            resolve_block(e->block);
            if (e->block && e->block->trailing) {
                return resolve_expr(e->block->trailing);
            }
            return tc_.void_type();
        case ExprKind::Spawn:
        case ExprKind::Comptime:
            resolve_block(e->block);
            if (e->block && e->block->trailing) {
                return resolve_expr(e->block->trailing);
            }
            return tc_.void_type();
        case ExprKind::Reflect:
            return tc_.void_type();
        case ExprKind::Await:
            return resolve_expr(e->lhs);
        case ExprKind::Tuple: {
            std::vector<TypePtr> args;
            for (ExprPtr a : e->args) args.push_back(resolve_expr(a));
            return tc_.make_tuple(std::move(args));
        }
        case ExprKind::ArrayLit: {
            TypePtr elem = tc_.make_error();
            if (!e->args.empty()) elem = resolve_expr(e->args[0]);
            return tc_.make_array(elem, e->args.size());
        }
    }
    return tc_.make_error();
}

void Resolver::resolve_pattern(PatternPtr p, TypePtr expected) {
    if (!p) return;
    (void)expected;
    switch (p->kind) {
        case PatternKind::Wildcard:
            break;
        case PatternKind::Binding: {
            Decl d;
            d.kind  = Decl::Kind::Binding;
            d.name  = p->name;
            d.type  = expected ? expected : tc_.make_error();
            d.range = p->range;
            d.slot  = next_slot_++;
            Decl* ptr = arena_.construct<Decl>(std::move(d));
            scopes_.back().names[p->name] = ptr;
            break;
        }
        case PatternKind::Bool:
        case PatternKind::Int:
        case PatternKind::String:
        case PatternKind::Char:
            break;
        case PatternKind::Variant: {
            // Look up the variant name in top_level.
            if (!p->path.empty()) {
                StrId first = p->path[0];
                auto it = top_level_.find(first);
                if (it == top_level_.end()) {
                    diag_.error(p->range,
                        std::string("unknown variant '") +
                        std::string(intern_.get(first)) + "'");
                }
            }
            for (PatternPtr s : p->subpatterns) resolve_pattern(s);
            break;
        }
        case PatternKind::Tuple:
            for (PatternPtr s : p->subpatterns) resolve_pattern(s);
            break;
        case PatternKind::Struct:
            for (const auto& f : p->fields) {
                if (f.sub) resolve_pattern(f.sub);
            }
            break;
        case PatternKind::As: {
            resolve_pattern(p->inner);
            // Register the binding.
            Decl d;
            d.kind  = Decl::Kind::Binding;
            d.name  = p->name;
            d.type  = expected ? expected : tc_.make_error();
            d.range = p->range;
            d.slot  = next_slot_++;
            Decl* ptr = arena_.construct<Decl>(std::move(d));
            scopes_.back().names[p->name] = ptr;
            break;
        }
        case PatternKind::Or:
            for (PatternPtr alt : p->alternatives) {
                resolve_pattern(alt);
            }
            break;
        case PatternKind::Range:
            // No bindings to register.
            break;
    }
}

} // namespace tether::resolve
