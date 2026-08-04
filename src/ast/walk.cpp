// ast/walk.cpp — depth-first AST traversal
//
// The walk helpers visit every node in the AST, calling the
// appropriate Visitor method. They do not short-circuit; subclasses
// that want to stop early should throw or set a flag.

#include "ast/nodes.hpp"

namespace tether::ast {

void walk(const Module& m, Visitor& v) {
    v.visit_module(m);
    for (ItemPtr item : m.items) {
        if (item) walk(*item, v);
    }
}

void walk(const Item& i, Visitor& v) {
    v.visit_item(i);
    if (i.inner) walk(*i.inner, v);
    if (i.impl_type)  walk(*i.impl_type,  v);
    if (i.impl_trait) walk(*i.impl_trait, v);
    for (const auto& tp : i.type_params) {
        (void)tp; // bounds are paths; no further walking needed.
    }
    for (const auto& p : i.params) {
        if (p.type) walk(*p.type, v);
    }
    if (i.return_type) walk(*i.return_type, v);
    if (i.body) walk(*i.body, v);
    for (const auto& f : i.fields) {
        if (f.type) walk(*f.type, v);
    }
    for (const auto& var : i.variants) {
        for (TypePtr t : var.args) if (t) walk(*t, v);
    }
    for (ItemPtr m : i.trait_members) if (m) walk(*m, v);
    for (ItemPtr m : i.impl_members)  if (m) walk(*m, v);
    if (i.alias_type)  walk(*i.alias_type,  v);
    if (i.const_type)  walk(*i.const_type,  v);
    if (i.const_value) walk(*i.const_value, v);
    if (i.extern_decl) walk(*i.extern_decl, v);
}

void walk(const Type& t, Visitor& v) {
    v.visit_type(t);
    if (t.base) walk(*t.base, v);
    for (TypePtr a : t.args) if (a) walk(*a, v);
    if (t.length) walk(*t.length, v);
    if (t.return_type) walk(*t.return_type, v);
}

void walk(const Pattern& p, Visitor& v) {
    v.visit_pattern(p);
    if (p.inner) walk(*p.inner, v);
    for (PatternPtr s : p.subpatterns) if (s) walk(*s, v);
    for (const auto& f : p.fields) if (f.sub) walk(*f.sub, v);
}

void walk(const Expr& e, Visitor& v) {
    v.visit_expr(e);
    if (e.lhs) walk(*e.lhs, v);
    if (e.rhs) walk(*e.rhs, v);
    for (ExprPtr a : e.args) if (a) walk(*a, v);
    if (e.index) walk(*e.index, v);
    if (e.block) walk(*e.block, v);
    if (e.cond)        walk(*e.cond,        v);
    if (e.then_branch) walk(*e.then_branch, v);
    if (e.else_branch) walk(*e.else_branch, v);
    if (e.body)        walk(*e.body,        v);
    if (e.iterable)    walk(*e.iterable,    v);
    if (e.return_value) walk(*e.return_value, v);
    if (e.alloc_value)  walk(*e.alloc_value,  v);
    for (const auto& arm : e.arms) {
        if (arm.pattern) walk(*arm.pattern, v);
        if (arm.body)    walk(*arm.body,    v);
    }
    if (e.element_type) walk(*e.element_type, v);
}

void walk(const Stmt& s, Visitor& v) {
    v.visit_stmt(s);
    if (s.let_type)  walk(*s.let_type,  v);
    if (s.let_value) walk(*s.let_value, v);
    if (s.expr)      walk(*s.expr,      v);
    if (s.block)     walk(*s.block,     v);
}

void walk(const Block& b, Visitor& v) {
    v.visit_block(b);
    for (StmtPtr s : b.stmts) if (s) walk(*s, v);
    if (b.trailing) walk(*b.trailing, v);
}

} // namespace tether::ast
