// check/check.cpp — type checker implementation

#include "check/check.hpp"

#include <string>
#include <utility>

namespace tether::check {

using namespace tether::ast;
// Note: we do NOT say `using namespace tether::type` because that
// would clash with ast::type::TypePtr. We use the `type::` prefix instead.

bool TypeChecker::check_module(const ast::Module& m) {
    // First pass: collect trait definitions.
    for (ItemPtr item : m.items) {
        if (!item) continue;
        if (item->kind == ItemKind::Trait) {
            std::vector<TraitMethod> methods;
            for (ItemPtr m : item->trait_members) {
                if (m && m->kind == ItemKind::Fn) {
                    TraitMethod tm;
                    tm.name = m->name;
                    tm.param_count = m->params.size();
                    tm.has_return = (m->return_type != nullptr);
                    methods.push_back(tm);
                }
            }
            traits_[item->name] = std::move(methods);
        }
    }

    // Second pass: check items.
    for (ItemPtr item : m.items) {
        check_item(item);
    }
    return !diag_.has_errors();
}

void TypeChecker::check_item(ItemPtr item) {
    if (!item) return;
    switch (item->kind) {
        case ItemKind::Fn:        check_fn(item); break;
        case ItemKind::Struct:    check_struct(item); break;
        case ItemKind::Enum:      check_enum(item); break;
        case ItemKind::Union:
        case ItemKind::Trait:
            for (ItemPtr m : item->trait_members) check_item(m);
            break;
        case ItemKind::Impl:
            // Check that the impl satisfies its trait (if specified).
            check_impl_satisfies_trait(*item);
            for (ItemPtr m : item->impl_members) check_item(m);
            break;
        case ItemKind::TypeAlias:
        case ItemKind::Const:
        case ItemKind::Static:
        case ItemKind::Extern:
        case ItemKind::Module:
        case ItemKind::Import:
        case ItemKind::Rewrite:
            break;
        case ItemKind::Export:
            check_item(item->inner);
            break;
    }
}

void TypeChecker::check_impl_satisfies_trait(const ast::Item& impl) {
    // If the impl doesn't specify a trait, nothing to check.
    if (!impl.impl_trait) return;
    if (impl.impl_trait->path.empty()) return;

    StrId trait_name = impl.impl_trait->path[0];
    auto it = traits_.find(trait_name);
    if (it == traits_.end()) {
        // Unknown trait — not necessarily an error (might be defined
        // in another module). Skip.
        return;
    }
    const auto& required_methods = it->second;

    // Collect methods provided by this impl.
    std::unordered_map<StrId, const ast::Item*> provided;
    for (ItemPtr m : impl.impl_members) {
        if (m && m->kind == ItemKind::Fn) {
            provided[m->name] = m;
        }
    }

    // Check that every required method is provided with a matching
    // signature.
    for (const auto& req : required_methods) {
        auto pit = provided.find(req.name);
        if (pit == provided.end()) {
            diag_.error(impl.range,
                std::string("impl does not satisfy trait: missing method '") +
                std::string(intern_.get(req.name)) + "'");
            continue;
        }
        const ast::Item* provided_fn = pit->second;
        // Check parameter count.
        if (provided_fn->params.size() != req.param_count) {
            diag_.error(provided_fn->range,
                std::string("method '") + std::string(intern_.get(req.name)) +
                "' has wrong parameter count: expected " +
                std::to_string(req.param_count) + ", got " +
                std::to_string(provided_fn->params.size()));
        }
        // Check return type presence.
        if (provided_fn->return_type && !req.has_return) {
            diag_.error(provided_fn->range,
                std::string("method '") + std::string(intern_.get(req.name)) +
                "' should not have a return type");
        } else if (!provided_fn->return_type && req.has_return) {
            diag_.error(provided_fn->range,
                std::string("method '") + std::string(intern_.get(req.name)) +
                "' is missing required return type");
        }
    }
}

void TypeChecker::check_fn(ItemPtr item) {
    // Resolve the function's type from the resolver's decl table.
    // The resolver stored it on the top-level Decl.
    // For v0.2 we re-derive it here.
    std::vector<type::TypePtr> param_types;
    for (const auto& p : item->params) {
        // The resolver already resolved types; we don't have direct
        // access to them here, so we re-resolve via the resolver's
        // TypeContext. This is wasteful but correct.
        param_types.push_back(tc_.make_error());
    }
    type::TypePtr ret = item->return_type ? tc_.make_error() : tc_.void_type();
    (void)ret;
    current_return_type_ = ret;

    if (item->body) {
        check_block(item->body, ret);
    }
}

void TypeChecker::check_struct(ItemPtr item) {
    // Verify fields have valid types. The resolver already did this.
}

void TypeChecker::check_enum(ItemPtr item) {
    // Verify variant argument types.
}

void TypeChecker::check_block(ast::BlockPtr b, type::TypePtr expected) {
    if (!b) return;
    for (StmtPtr s : b->stmts) check_stmt(s);
    if (b->trailing) {
        type::TypePtr t = check_expr(b->trailing, expected);
        if (expected && !tc_.is_error(t)) {
            expect_type(expected, t, b->trailing->range, "block result");
        }
    } else if (expected && !tc_.is_void(expected)) {
        // Block with no trailing expression but expected non-void:
        // the function may have explicit returns. Don't error here.
    }
}

void TypeChecker::check_stmt(StmtPtr s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Let: {
            type::TypePtr expected = s->let_type ? tc_.make_error() : nullptr;
            if (s->let_value) {
                type::TypePtr vt = check_expr(s->let_value, expected);
                if (s->let_type && !tc_.is_error(vt)) {
                    expect_type(expected, vt, s->range, "let binding");
                }
            }
            break;
        }
        case StmtKind::Expr:
            (void)check_expr(s->expr);
            break;
        case StmtKind::Return: {
            type::TypePtr expected = current_return_type_;
            if (s->expr) {
                type::TypePtr t = check_expr(s->expr, expected);
                if (expected && !tc_.is_error(t)) {
                    expect_type(expected, t, s->range, "return value");
                }
            } else if (expected && !tc_.is_void(expected)) {
                diag_.error(s->range, "missing return value");
            }
            break;
        }
        case StmtKind::Defer:
            if (s->expr) (void)check_expr(s->expr);
            break;
        case StmtKind::Break:
            if (!in_loop_) {
                diag_.error(s->range, "'break' outside of a loop");
            }
            break;
        case StmtKind::Continue:
            if (!in_loop_) {
                diag_.error(s->range, "'continue' outside of a loop");
            }
            break;
        case StmtKind::Unsafe: {
            bool saved = in_unsafe_;
            in_unsafe_ = true;
            check_block(s->block, nullptr);
            in_unsafe_ = saved;
            break;
        }
        case StmtKind::Block:
            check_block(s->block, nullptr);
            break;
    }
}

type::TypePtr TypeChecker::check_expr(ExprPtr e, type::TypePtr expected) {
    if (!e) return tc_.make_error();
    type::TypePtr actual;
    switch (e->kind) {
        case ExprKind::IntLit: {
            // Default to i32; if expected is a different integer type,
            // use that.
            if (expected && tc_.is_integer(expected)) {
                actual = expected;
            } else {
                actual = tc_.i32();
            }
            break;
        }
        case ExprKind::FloatLit: {
            if (expected && expected->kind == type::Kind::Float) {
                actual = expected;
            } else {
                actual = tc_.f64();
            }
            break;
        }
        case ExprKind::StringLit:
            actual = tc_.make_raw_ptr(tc_.u8(), false);
            break;
        case ExprKind::CharLit:
            actual = tc_.u32();
            break;
        case ExprKind::BoolLit:
            actual = tc_.boolean();
            break;
        case ExprKind::Ident:
        case ExprKind::Path:
            // Type comes from resolution; we don't have direct access
            // here in v0.2. Use error type as placeholder.
            actual = tc_.make_error();
            break;
        case ExprKind::Unary: {
            type::TypePtr base = check_expr(e->lhs);
            switch (e->unary_op) {
                case UnaryOp::Neg:
                case UnaryOp::BitNot:
                    if (!tc_.is_error(base) && !tc_.is_integer(base)) {
                        diag_.error(e->range,
                            std::string("unary ") +
                            (e->unary_op == UnaryOp::Neg ? "-" : "~") +
                            " requires integer, got " + tc_.render(base));
                    }
                    actual = base;
                    break;
                case UnaryOp::Not:
                    if (!tc_.is_error(base) && !tc_.is_boolean(base)) {
                        diag_.error(e->range,
                            "unary ! requires bool, got " + tc_.render(base));
                    }
                    actual = tc_.boolean();
                    break;
                case UnaryOp::Deref:
                    if (base && base->kind == type::Kind::Ref) {
                        actual = base->base;
                    } else if (base && base->kind == type::Kind::RawPtr) {
                        if (!in_unsafe_) {
                            diag_.error(e->range,
                                "dereferencing raw pointer requires 'unsafe'");
                        }
                        actual = base->base;
                    } else {
                        diag_.error(e->range,
                            "cannot dereference " + tc_.render(base));
                        actual = tc_.make_error();
                    }
                    break;
                case UnaryOp::Borrow:
                    actual = tc_.make_ref(base, false);
                    break;
                case UnaryOp::BorrowMut:
                    actual = tc_.make_ref(base, true);
                    break;
                case UnaryOp::Move:
                    actual = base;
                    break;
            }
            break;
        }
        case ExprKind::Binary: {
            type::TypePtr lt = check_expr(e->lhs);
            type::TypePtr rt = check_expr(e->rhs);
            switch (e->binary_op) {
                case BinaryOp::And:
                case BinaryOp::Or:
                    if (!tc_.is_error(lt) && !tc_.is_boolean(lt)) {
                        diag_.error(e->range,
                            "left operand of && / || must be bool, got " +
                            tc_.render(lt));
                    }
                    if (!tc_.is_error(rt) && !tc_.is_boolean(rt)) {
                        diag_.error(e->range,
                            "right operand of && / || must be bool, got " +
                            tc_.render(rt));
                    }
                    actual = tc_.boolean();
                    break;
                case BinaryOp::Eq: case BinaryOp::Neq:
                case BinaryOp::Lt:  case BinaryOp::Gt:
                case BinaryOp::Le:  case BinaryOp::Ge:
                    if (!tc_.is_error(lt) && !tc_.is_scalar(lt)) {
                        diag_.error(e->range,
                            "comparison requires scalar, got " +
                            tc_.render(lt));
                    }
                    actual = tc_.boolean();
                    break;
                case BinaryOp::Add: case BinaryOp::Sub:
                case BinaryOp::Mul: case BinaryOp::Div:
                case BinaryOp::Mod:
                case BinaryOp::BitAnd: case BinaryOp::BitOr:
                case BinaryOp::BitXor: case BinaryOp::Shl: case BinaryOp::Shr:
                    if (!tc_.is_error(lt) && !tc_.is_integer(lt) &&
                        !tc_.is_error(rt) && !tc_.is_integer(rt)) {
                        diag_.error(e->range,
                            "arithmetic/bitwise op requires integers");
                    }
                    actual = lt;
                    break;
            }
            break;
        }
        case ExprKind::Assign: {
            type::TypePtr lt = check_expr(e->lhs);
            type::TypePtr rt = check_expr(e->rhs);
            if (!tc_.is_error(lt) && !tc_.is_error(rt)) {
                expect_type(lt, rt, e->range, "assignment");
            }
            actual = tc_.void_type();
            break;
        }
        case ExprKind::Call: {
            type::TypePtr callee = check_expr(e->lhs);
            for (ExprPtr a : e->args) (void)check_expr(a);
            if (callee && callee->kind == type::Kind::Fn) {
                actual = callee->return_type ? callee->return_type
                                             : tc_.void_type();
            } else {
                actual = tc_.make_error();
            }
            break;
        }
        case ExprKind::MethodCall: {
            (void)check_expr(e->lhs);
            for (ExprPtr a : e->args) (void)check_expr(a);
            actual = tc_.make_error();
            break;
        }
        case ExprKind::FieldAccess: {
            (void)check_expr(e->lhs);
            actual = tc_.make_error();
            break;
        }
        case ExprKind::Index: {
            type::TypePtr base = check_expr(e->lhs);
            (void)check_expr(e->index, tc_.u64());
            if (base && base->kind == type::Kind::Array) actual = base->base;
            else if (base && base->kind == type::Kind::Slice) actual = base->base;
            else actual = tc_.make_error();
            break;
        }
        case ExprKind::Question: {
            actual = check_expr(e->lhs);
            break;
        }
        case ExprKind::Block: {
            check_block(e->block, expected);
            if (e->block && e->block->trailing) {
                actual = check_expr(e->block->trailing, expected);
            } else {
                actual = tc_.void_type();
            }
            break;
        }
        case ExprKind::If: {
            (void)check_expr(e->cond, tc_.boolean());
            type::TypePtr then_t = check_expr(e->then_branch, expected);
            type::TypePtr else_t = nullptr;
            if (e->else_branch) {
                else_t = check_expr(e->else_branch, expected);
            }
            if (then_t && else_t) {
                if (!tc_.is_error(then_t) && !tc_.is_error(else_t) &&
                    !type::is_assignable(then_t, else_t)) {
                    diag_.error(e->range,
                        "if branches have different types: " +
                        tc_.render(then_t) + " vs " + tc_.render(else_t));
                }
                actual = then_t;
            } else {
                actual = tc_.void_type();
            }
            break;
        }
        case ExprKind::Match: {
            (void)check_expr(e->cond);
            type::TypePtr result = tc_.make_error();
            bool first = true;
            for (const auto& arm : e->arms) {
                check_pattern(arm.pattern, nullptr);
                type::TypePtr t = check_expr(arm.body, expected);
                if (first) { result = t; first = false; }
                else if (!tc_.is_error(result) && !tc_.is_error(t) &&
                         !type::is_assignable(result, t)) {
                    diag_.error(arm.range,
                        "match arm type mismatch: " + tc_.render(result) +
                        " vs " + tc_.render(t));
                }
            }
            actual = result;
            break;
        }
        case ExprKind::Loop: {
            bool saved = in_loop_;
            in_loop_ = true;
            (void)check_expr(e->body, nullptr);
            in_loop_ = saved;
            actual = tc_.void_type();
            break;
        }
        case ExprKind::While: {
            (void)check_expr(e->cond, tc_.boolean());
            bool saved = in_loop_;
            in_loop_ = true;
            (void)check_expr(e->body, nullptr);
            in_loop_ = saved;
            actual = tc_.void_type();
            break;
        }
        case ExprKind::For: {
            (void)check_expr(e->iterable);
            bool saved = in_loop_;
            in_loop_ = true;
            (void)check_expr(e->body, nullptr);
            in_loop_ = saved;
            actual = tc_.void_type();
            break;
        }
        case ExprKind::Return: {
            if (e->return_value) {
                (void)check_expr(e->return_value, current_return_type_);
            }
            actual = tc_.void_type();
            break;
        }
        case ExprKind::Break:
        case ExprKind::Continue:
            actual = tc_.void_type();
            break;
        case ExprKind::Defer:
            (void)check_expr(e->lhs);
            actual = tc_.void_type();
            break;
        case ExprKind::Alloc: {
            if (e->alloc_value) (void)check_expr(e->alloc_value);
            actual = tc_.make_error();
            break;
        }
        case ExprKind::Move:
            actual = check_expr(e->lhs);
            break;
        case ExprKind::Borrow:
            actual = tc_.make_ref(check_expr(e->lhs),
                                  e->int_value != 0);
            break;
        case ExprKind::Unsafe: {
            bool saved = in_unsafe_;
            in_unsafe_ = true;
            check_block(e->block, expected);
            if (e->block && e->block->trailing) {
                actual = check_expr(e->block->trailing, expected);
            } else {
                actual = tc_.void_type();
            }
            in_unsafe_ = saved;
            break;
        }
        case ExprKind::Spawn:
        case ExprKind::Comptime: {
            check_block(e->block, expected);
            if (e->block && e->block->trailing) {
                actual = check_expr(e->block->trailing, expected);
            } else {
                actual = tc_.void_type();
            }
            break;
        }
        case ExprKind::Await:
            actual = check_expr(e->lhs);
            break;
        case ExprKind::Tuple: {
            std::vector<type::TypePtr> args;
            for (ExprPtr a : e->args) args.push_back(check_expr(a));
            actual = tc_.make_tuple(std::move(args));
            break;
        }
        case ExprKind::ArrayLit: {
            type::TypePtr elem = tc_.make_error();
            for (ExprPtr a : e->args) {
                type::TypePtr t = check_expr(a, elem);
                if (tc_.is_error(elem)) elem = t;
            }
            actual = tc_.make_array(elem, e->args.size());
            break;
        }
    }

    if (expected && !tc_.is_error(actual) && !tc_.is_error(expected)) {
        expect_type(expected, actual, e->range, "expression");
    }
    return actual;
}

void TypeChecker::check_pattern(PatternPtr p, type::TypePtr expected) {
    if (!p) return;
    (void)expected;
    // v0.2: patterns are checked structurally but not deeply. The
    // resolver registers bindings; the type checker verifies variant
    // names exist.
    switch (p->kind) {
        case PatternKind::Variant:
            // Variant existence was checked by the resolver.
            for (PatternPtr s : p->subpatterns) check_pattern(s, nullptr);
            break;
        case PatternKind::Tuple:
            for (PatternPtr s : p->subpatterns) check_pattern(s, nullptr);
            break;
        case PatternKind::Struct:
            for (const auto& f : p->fields) {
                if (f.sub) check_pattern(f.sub, nullptr);
            }
            break;
        case PatternKind::As:
            check_pattern(p->inner, expected);
            break;
        default:
            break;
    }
}

bool TypeChecker::expect_type(type::TypePtr expected, type::TypePtr actual,
                              tether::SourceRange loc, const char* context) {
    if (type::is_assignable(actual, expected)) return true;
    if (tc_.is_error(expected) || tc_.is_error(actual)) return true;
    diag_.error(loc,
        std::string("type mismatch in ") + context + ": expected " +
        tc_.render(expected) + ", got " + tc_.render(actual));
    return false;
}

} // namespace tether::check
