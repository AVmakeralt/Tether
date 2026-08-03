// ast/printer.cpp — pretty-printer implementation

#include "ast/printer.hpp"

#include <ostream>
#include <string_view>

namespace tether::ast {

namespace {

const char* type_kind_name(TypeKind k) {
    switch (k) {
        case TypeKind::Named:    return "named";
        case TypeKind::Ref:      return "ref";
        case TypeKind::BorrowRef:return "borrow-ref";
        case TypeKind::RawPtr:   return "raw-ptr";
        case TypeKind::Array:    return "array";
        case TypeKind::Slice:    return "slice";
        case TypeKind::Tuple:    return "tuple";
        case TypeKind::Fn:       return "fn";
        case TypeKind::Infer:    return "infer";
    }
    return "?";
}

const char* pattern_kind_name(PatternKind k) {
    switch (k) {
        case PatternKind::Wildcard: return "wildcard";
        case PatternKind::Binding:  return "binding";
        case PatternKind::Bool:     return "bool";
        case PatternKind::Int:      return "int";
        case PatternKind::String:   return "string";
        case PatternKind::Char:     return "char";
        case PatternKind::Variant:  return "variant";
        case PatternKind::Tuple:    return "tuple";
        case PatternKind::Struct:   return "struct";
        case PatternKind::As:       return "as";
    }
    return "?";
}

const char* expr_kind_name(ExprKind k) {
    switch (k) {
        case ExprKind::IntLit:      return "int";
        case ExprKind::FloatLit:    return "float";
        case ExprKind::StringLit:   return "string";
        case ExprKind::CharLit:     return "char";
        case ExprKind::BoolLit:     return "bool";
        case ExprKind::Ident:       return "ident";
        case ExprKind::Path:        return "path";
        case ExprKind::Unary:       return "unary";
        case ExprKind::Binary:      return "binary";
        case ExprKind::Assign:      return "assign";
        case ExprKind::Call:        return "call";
        case ExprKind::MethodCall:  return "method-call";
        case ExprKind::FieldAccess: return "field";
        case ExprKind::Index:       return "index";
        case ExprKind::Question:    return "question";
        case ExprKind::Block:       return "block";
        case ExprKind::If:          return "if";
        case ExprKind::Match:       return "match";
        case ExprKind::Loop:        return "loop";
        case ExprKind::While:       return "while";
        case ExprKind::For:         return "for";
        case ExprKind::Return:      return "return";
        case ExprKind::Break:       return "break";
        case ExprKind::Continue:    return "continue";
        case ExprKind::Defer:       return "defer";
        case ExprKind::Alloc:       return "alloc";
        case ExprKind::Move:        return "move";
        case ExprKind::Borrow:      return "borrow";
        case ExprKind::Unsafe:      return "unsafe";
        case ExprKind::Spawn:       return "spawn";
        case ExprKind::Await:       return "await";
        case ExprKind::Tuple:       return "tuple";
        case ExprKind::ArrayLit:    return "array-lit";
    }
    return "?";
}

const char* stmt_kind_name(StmtKind k) {
    switch (k) {
        case StmtKind::Let:      return "let";
        case StmtKind::Expr:     return "expr";
        case StmtKind::Return:   return "return";
        case StmtKind::Defer:    return "defer";
        case StmtKind::Break:    return "break";
        case StmtKind::Continue: return "continue";
        case StmtKind::Unsafe:   return "unsafe";
        case StmtKind::Block:    return "block";
    }
    return "?";
}

const char* item_kind_name(ItemKind k) {
    switch (k) {
        case ItemKind::Module:    return "module";
        case ItemKind::Import:    return "import";
        case ItemKind::Export:    return "export";
        case ItemKind::Fn:        return "fn";
        case ItemKind::Struct:    return "struct";
        case ItemKind::Enum:      return "enum";
        case ItemKind::Union:     return "union";
        case ItemKind::Trait:     return "trait";
        case ItemKind::Impl:      return "impl";
        case ItemKind::TypeAlias: return "type-alias";
        case ItemKind::Const:     return "const";
        case ItemKind::Static:    return "static";
        case ItemKind::Extern:    return "extern";
    }
    return "?";
}

} // namespace

void Printer::newline() {
    out_ << '\n';
    for (int i = 0; i < indent_; ++i) out_ << "  ";
}

void Printer::emit(std::string_view s) {
    out_ << s;
}

void Printer::print_path(const std::vector<StrId>& path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) out_ << "::";
        out_ << intern_.get(path[i]);
    }
}

void Printer::print_type(TypePtr t) {
    if (!t) { out_ << "<null-type>"; return; }
    out_ << "(" << type_kind_name(t->kind);
    if (!t->path.empty()) { out_ << " "; print_path(t->path); }
    if (t->is_mut)        out_ << " :mut";
    if (t->is_borrow)     out_ << " :borrow";
    if (t->is_const_ptr)  out_ << " :const";
    if (t->region != kInvalidStrId) {
        out_ << " :region " << intern_.get(t->region);
    }
    if (t->base) { out_ << " :base "; print_type(t->base); }
    if (!t->args.empty()) {
        out_ << " :args (";
        for (std::size_t i = 0; i < t->args.size(); ++i) {
            if (i > 0) out_ << " ";
            print_type(t->args[i]);
        }
        out_ << ")";
    }
    if (t->length) {
        out_ << " :len ";
        print_expr(t->length);
    }
    if (t->return_type) {
        out_ << " :ret ";
        print_type(t->return_type);
    }
    out_ << ")";
}

void Printer::print_pattern(PatternPtr p) {
    if (!p) { out_ << "<null-pattern>"; return; }
    out_ << "(" << pattern_kind_name(p->kind);
    if (p->name != kInvalidStrId) {
        out_ << " :" << intern_.get(p->name);
        if (p->is_mut) out_ << " :mut";
    }
    if (p->kind == PatternKind::Bool) out_ << " " << (p->int_value ? "true" : "false");
    if (p->kind == PatternKind::Int)  out_ << " " << p->int_value;
    if (p->kind == PatternKind::Char) out_ << " 0x" << std::hex << p->int_value << std::dec;
    if (p->kind == PatternKind::String) {
        out_ << " \"" << intern_.get(p->str_value) << "\"";
    }
    if (!p->path.empty()) {
        out_ << " :path ";
        print_path(p->path);
    }
    if (!p->subpatterns.empty()) {
        out_ << " :sub (";
        for (std::size_t i = 0; i < p->subpatterns.size(); ++i) {
            if (i > 0) out_ << " ";
            print_pattern(p->subpatterns[i]);
        }
        out_ << ")";
    }
    if (!p->fields.empty()) {
        out_ << " :fields (";
        for (std::size_t i = 0; i < p->fields.size(); ++i) {
            if (i > 0) out_ << " ";
            const auto& f = p->fields[i];
            out_ << "(" << intern_.get(f.name);
            if (f.shorthand) out_ << " :shorthand";
            if (f.sub) { out_ << " "; print_pattern(f.sub); }
            out_ << ")";
        }
        out_ << ")";
    }
    if (p->has_rest) out_ << " :rest";
    if (p->inner) { out_ << " :inner "; print_pattern(p->inner); }
    out_ << ")";
}

const char* Printer::unary_op_name(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg:       return "neg";
        case UnaryOp::Not:       return "not";
        case UnaryOp::BitNot:    return "bit-not";
        case UnaryOp::Deref:     return "deref";
        case UnaryOp::Borrow:    return "borrow";
        case UnaryOp::BorrowMut: return "borrow-mut";
        case UnaryOp::Move:      return "move";
    }
    return "?";
}

const char* Printer::binary_op_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:    return "+";
        case BinaryOp::Sub:    return "-";
        case BinaryOp::Mul:    return "*";
        case BinaryOp::Div:    return "/";
        case BinaryOp::Mod:    return "%";
        case BinaryOp::Eq:     return "==";
        case BinaryOp::Neq:    return "!=";
        case BinaryOp::Lt:     return "<";
        case BinaryOp::Gt:     return ">";
        case BinaryOp::Le:     return "<=";
        case BinaryOp::Ge:     return ">=";
        case BinaryOp::And:    return "&&";
        case BinaryOp::Or:     return "||";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr:  return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::Shl:    return "<<";
        case BinaryOp::Shr:    return ">>";
    }
    return "?";
}

const char* Printer::assign_op_name(AssignOp op) {
    switch (op) {
        case AssignOp::Assign:    return "=";
        case AssignOp::AddAssign: return "+=";
        case AssignOp::SubAssign: return "-=";
        case AssignOp::MulAssign: return "*=";
        case AssignOp::DivAssign: return "/=";
        case AssignOp::ModAssign: return "%=";
    }
    return "?";
}

void Printer::print_expr(ExprPtr e) {
    if (!e) { out_ << "<null-expr>"; return; }
    out_ << "(" << expr_kind_name(e->kind);
    switch (e->kind) {
        case ExprKind::IntLit:
            out_ << " " << e->int_value;
            break;
        case ExprKind::FloatLit:
            out_ << " " << e->float_value;
            break;
        case ExprKind::StringLit:
            out_ << " \"" << intern_.get(e->str_value) << "\"";
            break;
        case ExprKind::CharLit:
            out_ << " 0x" << std::hex << e->int_value << std::dec;
            break;
        case ExprKind::BoolLit:
            out_ << " " << (e->int_value ? "true" : "false");
            break;
        case ExprKind::Ident:
            out_ << " " << intern_.get(e->path[0]);
            break;
        case ExprKind::Path:
            out_ << " ";
            print_path(e->path);
            break;
        case ExprKind::Unary:
            out_ << " :" << unary_op_name(e->unary_op) << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Binary:
            out_ << " :" << binary_op_name(e->binary_op) << " ";
            print_expr(e->lhs);
            out_ << " ";
            print_expr(e->rhs);
            break;
        case ExprKind::Assign:
            out_ << " :" << assign_op_name(e->assign_op) << " ";
            print_expr(e->lhs);
            out_ << " ";
            print_expr(e->rhs);
            break;
        case ExprKind::Call:
            out_ << " :callee ";
            print_expr(e->lhs);
            if (!e->args.empty()) {
                out_ << " :args (";
                for (std::size_t i = 0; i < e->args.size(); ++i) {
                    if (i > 0) out_ << " ";
                    print_expr(e->args[i]);
                }
                out_ << ")";
            }
            break;
        case ExprKind::MethodCall:
            out_ << " :recv ";
            print_expr(e->lhs);
            out_ << " :method " << intern_.get(e->method_name);
            if (!e->args.empty()) {
                out_ << " :args (";
                for (std::size_t i = 0; i < e->args.size(); ++i) {
                    if (i > 0) out_ << " ";
                    print_expr(e->args[i]);
                }
                out_ << ")";
            }
            break;
        case ExprKind::FieldAccess:
            out_ << " :recv ";
            print_expr(e->lhs);
            out_ << " :field " << intern_.get(e->field_name);
            break;
        case ExprKind::Index:
            out_ << " :arr ";
            print_expr(e->lhs);
            out_ << " :idx ";
            print_expr(e->index);
            break;
        case ExprKind::Question:
            out_ << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Block:
            out_ << " ";
            print_block(e->block);
            break;
        case ExprKind::If:
            out_ << " :cond ";
            print_expr(e->cond);
            out_ << " :then ";
            print_expr(e->then_branch);
            if (e->else_branch) {
                out_ << " :else ";
                print_expr(e->else_branch);
            }
            break;
        case ExprKind::Match:
            out_ << " :scrutinee ";
            print_expr(e->cond);
            out_ << " :arms (";
            ++indent_;
            for (const auto& arm : e->arms) {
                newline();
                print_match_arm(arm);
            }
            --indent_;
            newline();
            out_ << ")";
            break;
        case ExprKind::Loop:
            out_ << " :body ";
            print_expr(e->body);
            break;
        case ExprKind::While:
            out_ << " :cond ";
            print_expr(e->cond);
            out_ << " :body ";
            print_expr(e->body);
            break;
        case ExprKind::For:
            out_ << " :var " << intern_.get(e->loop_var);
            out_ << " :iter ";
            print_expr(e->iterable);
            out_ << " :body ";
            print_expr(e->body);
            break;
        case ExprKind::Return:
            if (e->return_value) { out_ << " "; print_expr(e->return_value); }
            break;
        case ExprKind::Break:
        case ExprKind::Continue:
            break;
        case ExprKind::Defer:
            out_ << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Alloc: {
            const char* tgt =
                e->alloc_target == Expr::AllocTarget::Arena ? "arena"
              : e->alloc_target == Expr::AllocTarget::Heap  ? "heap"
              : "named";
            out_ << " :" << tgt;
            if (e->alloc_arena_name != kInvalidStrId) {
                out_ << " " << intern_.get(e->alloc_arena_name);
            }
            if (e->alloc_value) {
                out_ << " :value ";
                print_expr(e->alloc_value);
            }
            break;
        }
        case ExprKind::Move:
            out_ << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Borrow:
            out_ << " :" << (e->int_value ? "mut" : "shared") << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Unsafe:
            out_ << " ";
            print_block(e->block);
            break;
        case ExprKind::Spawn:
            out_ << " ";
            print_expr(e->body);
            break;
        case ExprKind::Await:
            out_ << " ";
            print_expr(e->lhs);
            break;
        case ExprKind::Tuple:
            out_ << " :elems (";
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                if (i > 0) out_ << " ";
                print_expr(e->args[i]);
            }
            out_ << ")";
            break;
        case ExprKind::ArrayLit:
            out_ << " :elems (";
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                if (i > 0) out_ << " ";
                print_expr(e->args[i]);
            }
            if (e->element_type) {
                out_ << ") :type ";
                print_type(e->element_type);
            } else {
                out_ << ")";
            }
            break;
    }
    out_ << ")";
}

void Printer::print_stmt(StmtPtr s) {
    if (!s) { out_ << "<null-stmt>"; return; }
    out_ << "(" << stmt_kind_name(s->kind);
    switch (s->kind) {
        case StmtKind::Let:
            out_ << " " << intern_.get(s->let_name);
            if (s->let_is_mut) out_ << " :mut";
            if (s->let_type) { out_ << " :type "; print_type(s->let_type); }
            if (s->let_value) { out_ << " = "; print_expr(s->let_value); }
            break;
        case StmtKind::Expr:
            out_ << " ";
            print_expr(s->expr);
            break;
        case StmtKind::Return:
            if (s->expr) { out_ << " "; print_expr(s->expr); }
            break;
        case StmtKind::Defer:
            out_ << " ";
            print_expr(s->expr);
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        case StmtKind::Unsafe:
            out_ << " ";
            print_block(s->block);
            break;
        case StmtKind::Block:
            out_ << " ";
            print_block(s->block);
            break;
    }
    out_ << ")";
}

void Printer::print_block(BlockPtr b) {
    if (!b) { out_ << "<null-block>"; return; }
    out_ << "(block";
    ++indent_;
    for (StmtPtr s : b->stmts) {
        newline();
        print_stmt(s);
    }
    if (b->trailing) {
        newline();
        out_ << "(trailing ";
        print_expr(b->trailing);
        out_ << ")";
    }
    --indent_;
    out_ << ")";
}

void Printer::print_field(const Field& f) {
    out_ << "(" << intern_.get(f.name) << " ";
    print_type(f.type);
    out_ << ")";
}

void Printer::print_variant(const Variant& v) {
    out_ << "(" << intern_.get(v.name);
    if (!v.args.empty()) {
        out_ << " (";
        for (std::size_t i = 0; i < v.args.size(); ++i) {
            if (i > 0) out_ << " ";
            print_type(v.args[i]);
        }
        out_ << ")";
    }
    out_ << ")";
}

void Printer::print_param(const Param& p) {
    out_ << "(";
    if (p.is_self) {
        out_ << "self";
        if (p.is_borrow) out_ << " :borrow";
        if (p.is_borrow_mut) out_ << " :borrow-mut";
    } else {
        out_ << intern_.get(p.name);
    }
    out_ << " ";
    print_type(p.type);
    if (p.is_variadic) out_ << " :variadic";
    out_ << ")";
}

void Printer::print_type_param(const TypeParam& tp) {
    out_ << "(" << intern_.get(tp.name);
    if (!tp.bounds.empty()) {
        out_ << " :bounds (";
        for (std::size_t i = 0; i < tp.bounds.size(); ++i) {
            if (i > 0) out_ << " ";
            out_ << "(";
            for (std::size_t j = 0; j < tp.bounds[i].size(); ++j) {
                if (j > 0) out_ << "::";
                out_ << intern_.get(tp.bounds[i][j]);
            }
            out_ << ")";
        }
        out_ << ")";
    }
    out_ << ")";
}

void Printer::print_match_arm(const MatchArm& a) {
    out_ << "(arm ";
    print_pattern(a.pattern);
    out_ << " ";
    print_expr(a.body);
    out_ << ")";
}

void Printer::print_item(const Item& i) {
    out_ << "(" << item_kind_name(i.kind);
    if (i.kind == ItemKind::Module || i.kind == ItemKind::Import) {
        out_ << " ";
        print_path(i.path);
        if (i.import_alias != kInvalidStrId) {
            out_ << " :as " << intern_.get(i.import_alias);
        }
        out_ << ")";
        return;
    }
    if (i.kind == ItemKind::Export) {
        out_ << " ";
        if (i.inner) print_item(*i.inner);
        out_ << ")";
        return;
    }
    if (i.kind == ItemKind::Extern) {
        if (i.ffi_header != kInvalidStrId) {
            out_ << " :header \"" << intern_.get(i.ffi_header) << "\"";
        }
        if (i.extern_decl) {
            out_ << " :decl ";
            print_item(*i.extern_decl);
        }
        out_ << ")";
        return;
    }

    if (i.name != kInvalidStrId) {
        out_ << " " << intern_.get(i.name);
    }
    if (!i.type_params.empty()) {
        out_ << " :type-params (";
        for (std::size_t k = 0; k < i.type_params.size(); ++k) {
            if (k > 0) out_ << " ";
            print_type_param(i.type_params[k]);
        }
        out_ << ")";
    }
    if (!i.params.empty()) {
        out_ << " :params (";
        for (std::size_t k = 0; k < i.params.size(); ++k) {
            if (k > 0) out_ << " ";
            print_param(i.params[k]);
        }
        out_ << ")";
    }
    if (i.return_type) {
        out_ << " :ret ";
        print_type(i.return_type);
    }
    if (!i.fields.empty()) {
        out_ << " :fields (";
        for (std::size_t k = 0; k < i.fields.size(); ++k) {
            if (k > 0) out_ << " ";
            print_field(i.fields[k]);
        }
        out_ << ")";
    }
    if (!i.variants.empty()) {
        out_ << " :variants (";
        for (std::size_t k = 0; k < i.variants.size(); ++k) {
            if (k > 0) out_ << " ";
            print_variant(i.variants[k]);
        }
        out_ << ")";
    }
    if (i.impl_type) {
        out_ << " :type ";
        print_type(i.impl_type);
    }
    if (i.impl_trait) {
        out_ << " :trait ";
        print_type(i.impl_trait);
    }
    if (i.alias_type) {
        out_ << " = ";
        print_type(i.alias_type);
    }
    if (i.const_type) {
        out_ << " :type ";
        print_type(i.const_type);
    }
    if (i.const_value) {
        out_ << " = ";
        print_expr(i.const_value);
    }
    if (i.body) {
        out_ << " ";
        print_block(i.body);
    }
    if (!i.trait_members.empty() || !i.impl_members.empty()) {
        const std::vector<ast::ItemPtr>& members =
            i.trait_members.empty() ? i.impl_members : i.trait_members;
        out_ << " :members (";
        ++indent_;
        for (ast::ItemPtr m : members) {
            newline();
            if (m) {
                print_item(*m);
            } else {
                out_ << "<null-member>";
            }
        }
        --indent_;
        newline();
        out_ << ")";
    }
    out_ << ")";
}

void Printer::print(const Module& m) {
    out_ << "(module";
    if (!m.module_path.empty()) {
        out_ << " ";
        print_path(m.module_path);
    }
    ++indent_;
    for (ItemPtr item : m.items) {
        if (!item) continue;
        newline();
        print_item(*item);
    }
    --indent_;
    out_ << ")\n";
}

} // namespace tether::ast
