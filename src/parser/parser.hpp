// parser/parser.hpp — recursive descent + Pratt parser
//
// The parser consumes a Token stream and produces an arena-allocated
// AST. It uses:
//
//   - Recursive descent for top-level items, statements, and types.
//   - Pratt parsing (operator-precedence climbing) for expressions.
//
// Error recovery: on an unexpected token, the parser reports a
// diagnostic, advances past the token, and tries to resynchronize at
// the next semicolon or closing brace. This allows multiple errors to
// be reported per parse.

#pragma once

#include "ast/nodes.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/tokens.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <vector>

namespace tether {

class Parser {
public:
    Parser(InternTable& intern, DiagnosticEmitter& diag, SourceManager& sm,
           Arena& arena, std::vector<Token> tokens)
        : intern_(intern), diag_(diag), sm_(sm), arena_(arena),
          tokens_(std::move(tokens)) {}

    // Parse a full module. Returns the module AST (never null even on
    // error — error nodes are represented as missing fields).
    ast::ModulePtr parse_module();

private:
    InternTable&        intern_;
    DiagnosticEmitter&  diag_;
    SourceManager&      sm_;
    Arena&              arena_;
    std::vector<Token>  tokens_;
    std::size_t         pos_ = 0;

    // ---- Token cursor ----
    const Token& peek(std::size_t ahead = 0) const;
    const Token& current() const { return peek(0); }
    Token consume();
    bool match(TokenKind k);
    bool check(TokenKind k) const { return current().kind == k; }
    bool check_any(std::initializer_list<TokenKind> kinds) const;
    Token expect(TokenKind k, const char* what);

    // Error recovery: advance until we hit one of the given tokens at
    // brace-depth zero (relative to where recovery started).
    void recover_to(std::initializer_list<TokenKind> stops);

    // ---- AST construction helpers ----
    template <typename T>
    const T* make(T value) {
        return arena_.construct<T>(std::move(value));
    }

    // ---- Module-level parsing ----
    ast::ItemPtr parse_item();
    ast::ItemPtr parse_module_decl();
    ast::ItemPtr parse_import_decl();
    ast::ItemPtr parse_export_decl();
    ast::ItemPtr parse_fn_decl(bool is_extern);
    ast::ItemPtr parse_struct_decl();
    ast::ItemPtr parse_enum_decl();
    ast::ItemPtr parse_union_decl();
    ast::ItemPtr parse_trait_decl();
    ast::ItemPtr parse_impl_decl();
    ast::ItemPtr parse_type_alias();
    ast::ItemPtr parse_const_decl(bool is_static);
    ast::ItemPtr parse_extern_decl();

    // ---- Type parsing ----
    ast::TypePtr parse_type();
    ast::TypePtr parse_primary_type();
    std::vector<StrId> parse_path();
    std::vector<ast::TypeParam> parse_type_params();
    std::vector<ast::Param> parse_params(bool allow_self);

    // ---- Statement parsing ----
    ast::BlockPtr parse_block();
    ast::StmtPtr  parse_stmt();
    ast::StmtPtr  parse_let_stmt();
    ast::StmtPtr  parse_unsafe_block();
    ast::StmtPtr  parse_defer_stmt();

    // ---- Expression parsing (Pratt) ----
    ast::ExprPtr parse_expr();
    ast::ExprPtr parse_assignment_expr();
    ast::ExprPtr parse_binary_expr(int min_prec);
    ast::ExprPtr parse_unary_expr();
    ast::ExprPtr parse_postfix_expr();
    ast::ExprPtr parse_primary_expr();
    ast::ExprPtr parse_if_expr();
    ast::ExprPtr parse_match_expr();
    ast::ExprPtr parse_loop_expr();
    ast::ExprPtr parse_while_expr();
    ast::ExprPtr parse_for_expr();
    ast::ExprPtr parse_spawn_expr();
    ast::ExprPtr parse_alloc_expr();
    ast::ExprPtr parse_block_as_expr_or_block();

    // Pratt table.
    struct PrecInfo {
        ast::BinaryOp bin_op;
        int           prec;
        bool          right_assoc;
    };
    bool try_binary_op(PrecInfo& out);

    // ---- Pattern parsing ----
    ast::PatternPtr parse_pattern();

    // ---- Utility ----
    StrId intern_current_str() const;
    std::vector<StrId> parse_path_after_first_ident();
};

} // namespace tether
