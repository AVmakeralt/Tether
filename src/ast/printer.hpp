// ast/printer.hpp — pretty-print an AST as S-expressions
//
// The pretty-printer is the primary output of tetherc in v0.1. It is
// deliberately verbose — every node and field is shown, so a human can
// verify the parser is doing the right thing.

#pragma once

#include "ast/nodes.hpp"
#include "support/intern.hpp"

#include <iosfwd>

namespace tether::ast {

class Printer {
public:
    Printer(std::ostream& out, const InternTable& intern)
        : out_(out), intern_(intern) {}

    void print(const Module& m);

private:
    std::ostream&          out_;
    const InternTable&     intern_;
    int                    indent_ = 0;

    void newline();
    void emit(std::string_view s);

    // Helpers.
    void print_path(const std::vector<StrId>& path);
    void print_type(TypePtr t);
    void print_pattern(PatternPtr p);
    void print_expr(ExprPtr e);
    void print_stmt(StmtPtr s);
    void print_block(BlockPtr b);
    void print_item(const Item& i);
    void print_field(const Field& f);
    void print_variant(const Variant& v);
    void print_param(const Param& p);
    void print_type_param(const TypeParam& tp);
    void print_match_arm(const MatchArm& a);

    const char* unary_op_name(UnaryOp op);
    const char* binary_op_name(BinaryOp op);
    const char* assign_op_name(AssignOp op);
};

} // namespace tether::ast
