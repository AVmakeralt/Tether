// parser/parser.cpp — parser implementation
//
// This is the largest single file in v0.1. It is intentionally
// straightforward — no fancy error recovery, no backtracking. The goal
// is correctness and readability.

#include "parser/parser.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

namespace tether {

// ============================================================================
// Token cursor
// ============================================================================

const Token& Parser::peek(std::size_t ahead) const {
    std::size_t i = pos_ + ahead;
    if (i >= tokens_.size()) {
        static const Token eof{TokenKind::Eof, {}, kInvalidStrId, 0, 0.0};
        return eof;
    }
    return tokens_[i];
}

Token Parser::consume() {
    Token t = current();
    if (pos_ < tokens_.size()) ++pos_;
    return t;
}

bool Parser::match(TokenKind k) {
    if (current().kind == k) {
        consume();
        return true;
    }
    return false;
}

bool Parser::check_any(std::initializer_list<TokenKind> kinds) const {
    for (TokenKind k : kinds) {
        if (current().kind == k) return true;
    }
    return false;
}

Token Parser::expect(TokenKind k, const char* what) {
    if (current().kind == k) {
        return consume();
    }
    const char* spelling = token_kind_spelling(k);
    std::string msg = "expected ";
    msg += what;
    if (spelling) {
        msg += " ('";
        msg += spelling;
        msg += "')";
    }
    msg += ", got '";
    if (const char* got = token_kind_spelling(current().kind)) {
        msg += got;
    } else {
        msg += token_kind_name(current().kind);
    }
    msg += "'";
    diag_.error(current().range, std::move(msg));
    return current();
}

void Parser::recover_to(std::initializer_list<TokenKind> stops) {
    int depth = 0;
    while (current().kind != TokenKind::Eof) {
        TokenKind k = current().kind;
        if (depth == 0) {
            for (TokenKind stop : stops) {
                if (k == stop) return;
            }
        }
        if (k == TokenKind::LBrace || k == TokenKind::LParen ||
            k == TokenKind::LBracket) {
            ++depth;
        } else if (k == TokenKind::RBrace || k == TokenKind::RParen ||
                   k == TokenKind::RBracket) {
            if (depth == 0) return;
            --depth;
        }
        consume();
    }
}

// ============================================================================
// Module
// ============================================================================

ast::ModulePtr Parser::parse_module() {
    ast::Module m;
    // Optional 'module foo::bar' at the top.
    if (check(TokenKind::KwModule)) {
        m.range = current().range;
        consume();
        m.module_path = parse_path();
        // The module decl may or may not have a trailing semicolon.
        match(TokenKind::Semicolon);
    }

    while (current().kind != TokenKind::Eof) {
        if (check(TokenKind::Semicolon)) { consume(); continue; }
        ast::ItemPtr item = parse_item();
        if (item) m.items.push_back(item);
        else { consume(); }
    }
    return make(std::move(m));
}

// ============================================================================
// Items
// ============================================================================

ast::ItemPtr Parser::parse_item() {
    // Parse attributes: @inline, @noalias, @cold, etc.
    std::vector<ast::Item::Attribute> attrs;
    while (check(TokenKind::At)) {
        ast::Item::Attribute attr;
        attr.range = current().range;
        consume(); // '@'
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected attribute name after '@'");
            break;
        }
        attr.name = current().str_id;
        consume();
        // Optional arguments: @inline(always) or @inline(always, cold)
        if (match(TokenKind::LParen)) {
            while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                attr.args.push_back(parse_expr());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen, "')' to close attribute args");
        }
        attrs.push_back(std::move(attr));
    }

    ast::ItemPtr item = parse_item_inner();
    if (item && !attrs.empty()) {
        const_cast<ast::Item*>(item)->attributes = std::move(attrs);
    }
    return item;
}

ast::ItemPtr Parser::parse_item_inner() {
    switch (current().kind) {
        case TokenKind::KwModule: return parse_module_decl();
        case TokenKind::KwImport: return parse_import_decl();
        case TokenKind::KwExport: return parse_export_decl();
        case TokenKind::KwFn:     return parse_fn_decl(false);
        case TokenKind::KwStruct: return parse_struct_decl();
        case TokenKind::KwEnum:   return parse_enum_decl();
        case TokenKind::KwUnion:  return parse_union_decl();
        case TokenKind::KwTrait:  return parse_trait_decl();
        case TokenKind::KwImpl:   return parse_impl_decl();
        case TokenKind::KwType:
        case TokenKind::KwAlias:  return parse_type_alias();
        case TokenKind::KwConst:  return parse_const_decl(false);
        case TokenKind::KwStatic: return parse_const_decl(true);
        case TokenKind::KwExtern: return parse_extern_decl();
        case TokenKind::KwFfi: {
            // 'ffi "header.h"' followed by extern decls.
            ast::Item item;
            item.kind  = ast::ItemKind::Extern;
            item.range = current().range;
            consume();
            if (check(TokenKind::StringLit)) {
                item.ffi_header = current().str_id;
                consume();
            }
            // One or more extern decls.
            if (check(TokenKind::KwExtern)) {
                item.extern_decl = parse_extern_decl();
            }
            return make(std::move(item));
        }
        default: {
            // Check for contextual 'rewrite' keyword.
            if (check(TokenKind::Ident) &&
                intern_.get(current().str_id) == "rewrite") {
                consume();
                ast::Item item;
                item.kind  = ast::ItemKind::Rewrite;
                item.range = current().range;
                if (!check(TokenKind::Ident)) {
                    diag_.error(current().range, "expected rewrite rule name");
                    return nullptr;
                }
                item.name = current().str_id;
                consume();
                expect(TokenKind::LBrace, "'{' to open rewrite body");
                while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
                    ast::RewriteArm arm;
                    arm.range = current().range;
                    arm.pattern = parse_expr();
                    expect(TokenKind::FatArrow, "'=>' in rewrite arm");
                    arm.replacement = parse_expr();
                    item.rewrite_arms.push_back(std::move(arm));
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RBrace, "'}' to close rewrite body");
                return make(std::move(item));
            }
            // Check for contextual 'macro' keyword.
            if (check(TokenKind::Ident) &&
                intern_.get(current().str_id) == "macro") {
                consume();
                ast::Item item;
                item.kind  = ast::ItemKind::Macro;
                item.range = current().range;
                if (!check(TokenKind::Ident)) {
                    diag_.error(current().range, "expected macro name");
                    return nullptr;
                }
                item.name = current().str_id;
                consume();
                expect(TokenKind::LBrace, "'{' to open macro body");
                while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
                    ast::RewriteArm arm;
                    arm.range = current().range;
                    arm.pattern = parse_expr();
                    expect(TokenKind::FatArrow, "'=>' in macro rule");
                    arm.replacement = parse_expr();
                    item.macro_rules.push_back(std::move(arm));
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RBrace, "'}' to close macro body");
                return make(std::move(item));
            }
            // Check for contextual 'effect' keyword.
            if (check(TokenKind::Ident) &&
                intern_.get(current().str_id) == "pure") {
                // pure fn ... — mark as pure effect
                consume();
                ast::ItemPtr fn = parse_fn_decl(false);
                if (fn) {
                    const_cast<ast::Item*>(fn)->effect = ast::Effect::Pure;
                }
                return fn;
            }
            if (check(TokenKind::Ident) &&
                intern_.get(current().str_id) == "io") {
                // io fn ... — mark as IO effect
                consume();
                ast::ItemPtr fn = parse_fn_decl(false);
                if (fn) {
                    const_cast<ast::Item*>(fn)->effect = ast::Effect::IO;
                }
                return fn;
            }
            std::string msg = "expected item, got '";
            if (const char* s = token_kind_spelling(current().kind)) msg += s;
            else msg += token_kind_name(current().kind);
            msg += "'";
            diag_.error(current().range, std::move(msg));
            recover_to({TokenKind::KwFn, TokenKind::KwStruct, TokenKind::KwEnum,
                        TokenKind::KwUnion, TokenKind::KwTrait, TokenKind::KwImpl,
                        TokenKind::KwType, TokenKind::KwAlias, TokenKind::KwConst,
                        TokenKind::KwStatic, TokenKind::KwExtern, TokenKind::KwFfi,
                        TokenKind::KwImport, TokenKind::KwExport, TokenKind::KwModule});
            return nullptr;
        }
    }
}

ast::ItemPtr Parser::parse_module_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Module;
    item.range = current().range;
    consume();
    item.path = parse_path();
    match(TokenKind::Semicolon);
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_import_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Import;
    item.range = current().range;
    consume();
    item.path = parse_path();
    if (check(TokenKind::SelfKw)) {
        // 'import foo::bar as baz' — but 'as' is not a keyword in our
        // 34-keyword set. Treat any trailing identifier as an alias.
        // For v0.1 we don't support aliases — the spec doesn't have 'as'.
        // Skip.
    }
    match(TokenKind::Semicolon);
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_export_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Export;
    item.range = current().range;
    consume();
    item.inner = parse_item();
    return make(std::move(item));
}

std::vector<StrId> Parser::parse_path() {
    std::vector<StrId> path;
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected identifier in path");
        return path;
    }
    path.push_back(current().str_id);
    consume();
    while (check(TokenKind::DoubleColon)) {
        consume();
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected identifier after '::'");
            break;
        }
        path.push_back(current().str_id);
        consume();
    }
    return path;
}

std::vector<StrId> Parser::parse_path_after_first_ident() {
    // Caller has already consumed the first ident.
    std::vector<StrId> path;
    // We assume the first ident's str_id was the last consumed token.
    // The parser state has pos_ past it. We reconstruct the path by
    // looking at tokens_.
    if (pos_ == 0 || tokens_[pos_ - 1].kind != TokenKind::Ident) {
        return path;
    }
    path.push_back(tokens_[pos_ - 1].str_id);
    while (check(TokenKind::DoubleColon)) {
        consume();
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected identifier after '::'");
            break;
        }
        path.push_back(current().str_id);
        consume();
    }
    return path;
}

std::vector<ast::TypeParam> Parser::parse_type_params() {
    std::vector<ast::TypeParam> out;
    if (!match(TokenKind::Lt)) return out;
    while (!check(TokenKind::Gt) && !check(TokenKind::Shr) && !check(TokenKind::Eof)) {
        ast::TypeParam tp;
        tp.range = current().range;
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected type parameter name");
            break;
        }
        tp.name = current().str_id;
        consume();
        if (match(TokenKind::Colon)) {
            // Bounds: comma-separated paths.
            do {
                std::vector<StrId> bound = parse_path();
                tp.bounds.push_back(std::move(bound));
            } while (match(TokenKind::Plus));
        }
        out.push_back(std::move(tp));
        if (!match(TokenKind::Comma)) break;
    }
    // '>>' may appear if the user wrote Vec<Vec<T>>. We treat it as
    // two '>' tokens for parsing simplicity by adjusting the token
    // stream is messy — instead, accept both Shr and Gt.
    if (check(TokenKind::Shr)) {
        // Replace the Shr with a single Gt by mutating tokens_.
        // Simpler: just consume it and synthesize a Gt.
        tokens_[pos_].kind = TokenKind::Gt;
        tokens_[pos_].range.end.offset = tokens_[pos_].range.start.offset + 1;
        // Leave pos_ alone; the match(Gt) below will consume it.
    }
    expect(TokenKind::Gt, "'>' to close type parameters");
    return out;
}

std::vector<ast::Param> Parser::parse_params(bool allow_self) {
    std::vector<ast::Param> out;
    expect(TokenKind::LParen, "'(' to open parameter list");
    bool seen_self = false;
    while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
        ast::Param p;
        p.range = current().range;

        if (allow_self && !seen_self && check(TokenKind::SelfKw)) {
            p.is_self = true;
            p.name = current().str_id;
            consume();
            seen_self = true;
            // 'self' may be followed by ': type' or be bare.
            if (match(TokenKind::Colon)) {
                p.type = parse_type();
            } else {
                // Bare self — type will be inferred by name resolution.
                // For now, leave null; the printer tolerates null.
            }
            out.push_back(std::move(p));
            if (match(TokenKind::Comma)) continue;
            break;
        }

        // borrow self / borrow mut self
        if (allow_self && !seen_self && check(TokenKind::KwBorrow)) {
            Token borrow_tok = current();
            consume();
            bool mut = match(TokenKind::KwMut);
            if (check(TokenKind::SelfKw)) {
                p.is_self      = true;
                p.is_borrow    = true;
                p.is_borrow_mut= mut;
                p.name         = current().str_id;
                p.range        = borrow_tok.range;
                consume();
                seen_self = true;
                if (match(TokenKind::Colon)) {
                    p.type = parse_type();
                }
                out.push_back(std::move(p));
                if (match(TokenKind::Comma)) continue;
                break;
            }
            // Else: 'borrow' as a type prefix. Backtrack by re-emitting
            // a token... not possible cleanly. Treat as parse error for
            // v0.1.
            diag_.error(borrow_tok.range, "'borrow' in param position must be followed by 'self'");
            break;
        }

        if (!check(TokenKind::Ident)) {
            // '...' for variadic extern params.
            // Now lexes as DotDot followed by Dot, or three Dots.
            if (check(TokenKind::DotDot)) {
                consume();
                if (match(TokenKind::Dot)) {
                    p.is_variadic = true;
                    out.push_back(std::move(p));
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
            }
            if (check(TokenKind::Dot)) {
                consume();
                if (match(TokenKind::Dot) && match(TokenKind::Dot)) {
                    p.is_variadic = true;
                    out.push_back(std::move(p));
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
            }
            diag_.error(current().range, "expected parameter name");
            break;
        }
        p.name = current().str_id;
        consume();
        expect(TokenKind::Colon, "':' after parameter name");
        p.type = parse_type();
        out.push_back(std::move(p));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "')' to close parameter list");
    return out;
}

ast::ItemPtr Parser::parse_fn_decl(bool is_extern) {
    ast::Item item;
    item.kind      = ast::ItemKind::Fn;
    item.is_extern = is_extern;
    item.range     = current().range;
    consume(); // 'fn'

    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected function name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();

    item.type_params = parse_type_params();
    item.params      = parse_params(/*allow_self=*/!is_extern);

    if (match(TokenKind::Arrow)) {
        item.return_type = parse_type();
    }

    // Optional where clause: where T: Trait, U: Trait2
    if (check(TokenKind::Ident) && intern_.get(current().str_id) == "where") {
        consume();
        while (!check(TokenKind::LBrace) && !check(TokenKind::Semicolon) &&
               !check(TokenKind::Eof)) {
            ast::Item::WhereClause wc;
            wc.type_bound = parse_type();
            if (match(TokenKind::Colon)) {
                do {
                    std::vector<StrId> bound = parse_path();
                    wc.trait_bounds.push_back(std::move(bound));
                } while (match(TokenKind::Plus));
            }
            item.where_clauses.push_back(std::move(wc));
            if (!match(TokenKind::Comma)) break;
        }
    }

    if (is_extern) {
        match(TokenKind::Semicolon);
    } else {
        if (check(TokenKind::LBrace)) {
            item.body = parse_block();
        } else {
            // Trait signature: no body.
            match(TokenKind::Semicolon);
        }
    }
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_struct_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Struct;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected struct name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    item.type_params = parse_type_params();
    expect(TokenKind::LBrace, "'{' to open struct body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::Field f;
        f.range = current().range;
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected field name");
            break;
        }
        f.name = current().str_id;
        consume();
        expect(TokenKind::Colon, "':' after field name");
        f.type = parse_type();
        item.fields.push_back(std::move(f));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "'}' to close struct body");
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_enum_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Enum;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected enum name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    item.type_params = parse_type_params();
    expect(TokenKind::LBrace, "'{' to open enum body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::Variant v;
        v.range = current().range;
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected variant name");
            break;
        }
        v.name = current().str_id;
        consume();
        if (match(TokenKind::LParen)) {
            while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                v.args.push_back(parse_type());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen, "')' to close variant args");
        }
        item.variants.push_back(std::move(v));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "'}' to close enum body");
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_union_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Union;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected union name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    item.type_params = parse_type_params();
    expect(TokenKind::LBrace, "'{' to open union body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::Field f;
        f.range = current().range;
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected field name");
            break;
        }
        f.name = current().str_id;
        consume();
        expect(TokenKind::Colon, "':' after field name");
        f.type = parse_type();
        item.fields.push_back(std::move(f));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "'}' to close union body");
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_trait_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Trait;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected trait name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    item.type_params = parse_type_params();
    expect(TokenKind::LBrace, "'{' to open trait body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::ItemPtr member = nullptr;
        if (check(TokenKind::KwFn)) {
            member = parse_fn_decl(false);
        } else if (check(TokenKind::KwConst)) {
            member = parse_const_decl(false);
        } else {
            diag_.error(current().range, "expected fn or const in trait body");
            recover_to({TokenKind::KwFn, TokenKind::KwConst, TokenKind::RBrace});
            continue;
        }
        if (member) {
            // Trait members are owned by the trait (not arena-duplicated).
            // We copy into the vector.
            item.trait_members.push_back(member);
        }
    }
    expect(TokenKind::RBrace, "'}' to close trait body");
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_impl_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Impl;
    item.range = current().range;
    consume();
    item.type_params = parse_type_params();
    item.impl_type   = parse_type();
    // 'for' is a keyword in Tether (loop construct), but it is reused
    // contextually as the impl-trait separator: `impl Trait for Type`.
    if (check(TokenKind::KwFor)) {
        consume();
        item.impl_trait = item.impl_type;
        item.impl_type  = parse_type();
    }
    expect(TokenKind::LBrace, "'{' to open impl body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::ItemPtr member = nullptr;
        if (check(TokenKind::KwFn)) {
            member = parse_fn_decl(false);
        } else if (check(TokenKind::KwConst)) {
            member = parse_const_decl(false);
        } else {
            diag_.error(current().range, "expected fn or const in impl body");
            recover_to({TokenKind::KwFn, TokenKind::KwConst, TokenKind::RBrace});
            continue;
        }
        if (member) item.impl_members.push_back(member);
    }
    expect(TokenKind::RBrace, "'}' to close impl body");
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_type_alias() {
    ast::Item item;
    item.kind  = ast::ItemKind::TypeAlias;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected type alias name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    item.type_params = parse_type_params();
    expect(TokenKind::Assign, "'=' in type alias");
    item.alias_type = parse_type();
    match(TokenKind::Semicolon);
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_const_decl(bool is_static) {
    ast::Item item;
    item.kind  = is_static ? ast::ItemKind::Static : ast::ItemKind::Const;
    item.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected const/static name");
        return nullptr;
    }
    item.name = current().str_id;
    consume();
    if (match(TokenKind::Colon)) {
        item.const_type = parse_type();
    }
    expect(TokenKind::Assign, "'=' in const/static");
    item.const_value = parse_expr();
    match(TokenKind::Semicolon);
    return make(std::move(item));
}

ast::ItemPtr Parser::parse_extern_decl() {
    ast::Item item;
    item.kind  = ast::ItemKind::Extern;
    item.range = current().range;
    consume(); // 'extern'

    // Optional calling convention: extern "C", extern "fastcall", etc.
    if (check(TokenKind::StringLit)) {
        std::string_view conv = intern_.get(current().str_id);
        if (conv == "C") {
            item.call_conv = ast::CallConv::C;
        } else if (conv == "fastcall") {
            item.call_conv = ast::CallConv::Fastcall;
        } else if (conv == "stdcall") {
            item.call_conv = ast::CallConv::Stdcall;
        } else if (conv == "vectorcall") {
            item.call_conv = ast::CallConv::Vectorcall;
        } else if (conv == "sysv") {
            item.call_conv = ast::CallConv::SysV;
        } else if (conv == "win64") {
            item.call_conv = ast::CallConv::Win64;
        } else {
            diag_.error(current().range,
                std::string("unknown calling convention '") +
                std::string(conv) + "'");
        }
        consume();
    }

    if (check(TokenKind::KwFn)) {
        item.extern_decl = parse_fn_decl(/*is_extern=*/true);
        // Propagate calling convention to the inner fn.
        if (item.extern_decl) {
            const_cast<ast::Item*>(item.extern_decl)->call_conv = item.call_conv;
        }
    } else if (check(TokenKind::KwStruct)) {
        item.extern_decl = parse_struct_decl();
    } else {
        diag_.error(current().range, "expected 'fn' after 'extern'");
    }
    return make(std::move(item));
}

// ============================================================================
// Types
// ============================================================================

ast::TypePtr Parser::parse_type() {
    ast::Type t;
    t.range = current().range;

    // 'borrow' is a keyword; 'ref' and 'mut' below are handled
    // contextually: 'ref' as an identifier, 'mut' as a keyword.
    //
    // borrow ref T        /  borrow mut ref T
    // mut ref T           /  ref T
    if (check(TokenKind::KwBorrow)) {
        consume();
        bool mut = match(TokenKind::KwMut);
        // Expect contextual 'ref'.
        if (!check(TokenKind::Ident) || intern_.get(current().str_id) != "ref") {
            diag_.error(current().range, "expected 'ref' after 'borrow' [mut]");
        } else {
            consume();
        }
        t.kind      = ast::TypeKind::BorrowRef;
        t.is_borrow = true;
        t.is_mut    = mut;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::Ident)) {
                diag_.error(current().range, "expected region name");
            } else {
                t.region = current().str_id;
                consume();
            }
            expect(TokenKind::RParen, "')' to close region");
        }
        t.base = parse_type();
        return make(std::move(t));
    }

    // 'mut ref T'  /  'mut ref(region) T'
    if (check(TokenKind::KwMut)) {
        consume();
        if (!check(TokenKind::Ident) || intern_.get(current().str_id) != "ref") {
            diag_.error(current().range, "expected 'ref' after 'mut'");
        } else {
            consume();
        }
        t.kind   = ast::TypeKind::Ref;
        t.is_mut = true;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::Ident)) {
                diag_.error(current().range, "expected region name");
            } else {
                t.region = current().str_id;
                consume();
            }
            expect(TokenKind::RParen, "')' to close region");
        }
        t.base = parse_type();
        return make(std::move(t));
    }
    // 'ref T' / 'ref(region) T'  — 'ref' is contextual, lexed as Ident.
    if (check(TokenKind::Ident) && intern_.get(current().str_id) == "ref") {
        consume();
        t.kind = ast::TypeKind::Ref;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::Ident)) {
                diag_.error(current().range, "expected region name");
            } else {
                t.region = current().str_id;
                consume();
            }
            expect(TokenKind::RParen, "')' to close region");
        }
        t.base = parse_type();
        return make(std::move(t));
    }

    return parse_primary_type();
}

ast::TypePtr Parser::parse_primary_type() {
    ast::Type t;
    t.range = current().range;

    switch (current().kind) {
        case TokenKind::Star: {
            // *const T  /  *mut T  — only valid in extern.
            consume();
            if (match(TokenKind::KwConst)) {
                t.is_const_ptr = true;
            } else if (match(TokenKind::KwMut)) {
                t.is_mut = true;
            } else {
                diag_.error(current().range, "expected 'const' or 'mut' after '*' in pointer type");
            }
            t.kind = ast::TypeKind::RawPtr;
            t.base = parse_type();
            return make(std::move(t));
        }
        case TokenKind::LParen: {
            consume();
            t.kind = ast::TypeKind::Tuple;
            while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                t.args.push_back(parse_type());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen, "')' to close tuple type");
            return make(std::move(t));
        }
        case TokenKind::LBracket: {
            consume();
            ast::TypePtr elem = parse_type();
            if (match(TokenKind::Semicolon)) {
                t.kind   = ast::TypeKind::Array;
                t.base   = elem;
                // Array length is an expression in Tether (so N can be a const).
                t.length = parse_expr();
            } else {
                t.kind = ast::TypeKind::Slice;
                t.base = elem;
            }
            expect(TokenKind::RBracket, "']' to close array/slice type");
            return make(std::move(t));
        }
        case TokenKind::Ident: {
            t.kind = ast::TypeKind::Named;
            t.path = parse_path();
            // Optional type arguments: Foo<T, U>
            if (check(TokenKind::Lt)) {
                consume();
                while (!check(TokenKind::Gt) && !check(TokenKind::Shr) && !check(TokenKind::Eof)) {
                    t.args.push_back(parse_type());
                    if (!match(TokenKind::Comma)) break;
                }
                if (check(TokenKind::Shr)) {
                    tokens_[pos_].kind = TokenKind::Gt;
                    tokens_[pos_].range.end.offset = tokens_[pos_].range.start.offset + 1;
                }
                expect(TokenKind::Gt, "'>' to close type arguments");
            }
            return make(std::move(t));
        }
        default: {
            std::string msg = "expected type, got '";
            if (const char* s = token_kind_spelling(current().kind)) msg += s;
            else msg += token_kind_name(current().kind);
            msg += "'";
            diag_.error(current().range, std::move(msg));
            // Return an Infer type so callers can continue.
            t.kind = ast::TypeKind::Infer;
            return make(std::move(t));
        }
    }
}

// ============================================================================
// Statements and blocks
// ============================================================================

ast::BlockPtr Parser::parse_block() {
    ast::Block b;
    b.range = current().range;
    expect(TokenKind::LBrace, "'{' to open block");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        // Allow stray ';' as empty statement.
        if (match(TokenKind::Semicolon)) continue;

        ast::StmtPtr s = parse_stmt();
        if (s) {
            // If this is an expression statement and the next token
            // starts a new statement, treat the expr as the block's
            // trailing expression only if it's the last thing before
            // '}' AND there's no trailing semicolon. For v0.1 we keep
            // things simple: every statement is a statement; trailing
            // expressions are detected when the next token is '}' and
            // the current stmt was an expr stmt.
            if (s->kind == ast::StmtKind::Expr && check(TokenKind::RBrace)) {
                b.trailing = s->expr;
            } else {
                b.stmts.push_back(s);
            }
        }
    }
    expect(TokenKind::RBrace, "'}' to close block");
    return make(std::move(b));
}

ast::StmtPtr Parser::parse_stmt() {
    switch (current().kind) {
        case TokenKind::KwLet:    return parse_let_stmt();
        case TokenKind::KwUnsafe: return parse_unsafe_block();
        case TokenKind::KwDefer:  return parse_defer_stmt();
        case TokenKind::KwReturn: {
            ast::Stmt s;
            s.kind  = ast::StmtKind::Return;
            s.range = current().range;
            consume();
            if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace)) {
                s.expr = parse_expr();
            }
            match(TokenKind::Semicolon);
            return make(std::move(s));
        }
        case TokenKind::KwBreak: {
            ast::Stmt s;
            s.kind  = ast::StmtKind::Break;
            s.range = current().range;
            consume();
            match(TokenKind::Semicolon);
            return make(std::move(s));
        }
        case TokenKind::KwContinue: {
            ast::Stmt s;
            s.kind  = ast::StmtKind::Continue;
            s.range = current().range;
            consume();
            match(TokenKind::Semicolon);
            return make(std::move(s));
        }
        case TokenKind::LBrace: {
            ast::Stmt s;
            s.kind  = ast::StmtKind::Block;
            s.range = current().range;
            s.block = parse_block();
            return make(std::move(s));
        }
        default: {
            // Expression statement.
            std::size_t pos_before = pos_;
            ast::Stmt s;
            s.kind  = ast::StmtKind::Expr;
            s.range = current().range;
            s.expr  = parse_expr();
            match(TokenKind::Semicolon);
            // Guarantee forward progress: if parse_expr() did not
            // consume any tokens (e.g. it hit an unexpected token and
            // returned a placeholder), consume one token to avoid an
            // infinite loop.
            if (pos_ == pos_before) {
                diag_.error(current().range,
                            std::string("unexpected token '") +
                            (token_kind_spelling(current().kind)
                                ? token_kind_spelling(current().kind)
                                : token_kind_name(current().kind)) +
                            "' in statement position");
                consume();
            }
            return make(std::move(s));
        }
    }
}

ast::StmtPtr Parser::parse_let_stmt() {
    ast::Stmt s;
    s.kind  = ast::StmtKind::Let;
    s.range = current().range;
    consume(); // 'let'
    // 'let mut name' — mutability comes before the binding name.
    s.let_is_mut = match(TokenKind::KwMut);
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected binding name in 'let'");
        return nullptr;
    }
    s.let_name = current().str_id;
    consume();
    if (match(TokenKind::Colon)) {
        s.let_type = parse_type();
    }
    if (match(TokenKind::Assign)) {
        s.let_value = parse_expr();
    }
    match(TokenKind::Semicolon);
    return make(std::move(s));
}

ast::StmtPtr Parser::parse_unsafe_block() {
    ast::Stmt s;
    s.kind  = ast::StmtKind::Unsafe;
    s.range = current().range;
    consume();
    s.block = parse_block();
    return make(std::move(s));
}

ast::StmtPtr Parser::parse_defer_stmt() {
    ast::Stmt s;
    s.kind  = ast::StmtKind::Defer;
    s.range = current().range;
    consume();
    s.expr = parse_expr();
    match(TokenKind::Semicolon);
    return make(std::move(s));
}

// ============================================================================
// Expressions (Pratt)
// ============================================================================

namespace {

struct PrecEntry {
    TokenKind  tok;
    ast::BinaryOp op;
    int        prec;
    bool       right_assoc;
};

const PrecEntry kPrecTable[] = {
    {TokenKind::Or,       ast::BinaryOp::Or,     1, false},
    {TokenKind::And,      ast::BinaryOp::And,    2, false},
    {TokenKind::BitOr,    ast::BinaryOp::BitOr,  3, false},
    {TokenKind::BitXor,   ast::BinaryOp::BitXor, 4, false},
    {TokenKind::BitAnd,   ast::BinaryOp::BitAnd, 5, false},
    {TokenKind::Eq,       ast::BinaryOp::Eq,     6, false},
    {TokenKind::Neq,      ast::BinaryOp::Neq,    6, false},
    {TokenKind::Lt,       ast::BinaryOp::Lt,     7, false},
    {TokenKind::Gt,       ast::BinaryOp::Gt,     7, false},
    {TokenKind::Le,       ast::BinaryOp::Le,     7, false},
    {TokenKind::Ge,       ast::BinaryOp::Ge,     7, false},
    {TokenKind::Shl,      ast::BinaryOp::Shl,    8, false},
    {TokenKind::Shr,      ast::BinaryOp::Shr,    8, false},
    {TokenKind::Plus,     ast::BinaryOp::Add,    9, false},
    {TokenKind::Minus,    ast::BinaryOp::Sub,    9, false},
    {TokenKind::Star,     ast::BinaryOp::Mul,   10, false},
    {TokenKind::Slash,    ast::BinaryOp::Div,   10, false},
    {TokenKind::Percent,  ast::BinaryOp::Mod,   10, false},
};

constexpr std::size_t kPrecTableSize =
    sizeof(kPrecTable) / sizeof(kPrecTable[0]);

} // namespace

bool Parser::try_binary_op(PrecInfo& out) {
    for (std::size_t i = 0; i < kPrecTableSize; ++i) {
        if (kPrecTable[i].tok == current().kind) {
            out.bin_op      = kPrecTable[i].op;
            out.prec        = kPrecTable[i].prec;
            out.right_assoc = kPrecTable[i].right_assoc;
            return true;
        }
    }
    return false;
}

ast::ExprPtr Parser::parse_expr() {
    return parse_assignment_expr();
}

ast::ExprPtr Parser::parse_assignment_expr() {
    ast::ExprPtr lhs = parse_binary_expr(0);
    if (!lhs) return nullptr;

    // Assignment is right-associative and lower-precedence than any
    // binary operator.
    ast::AssignOp op = ast::AssignOp::Assign;
    switch (current().kind) {
        case TokenKind::Assign:        op = ast::AssignOp::Assign;    break;
        case TokenKind::PlusAssign:    op = ast::AssignOp::AddAssign; break;
        case TokenKind::MinusAssign:   op = ast::AssignOp::SubAssign; break;
        case TokenKind::StarAssign:    op = ast::AssignOp::MulAssign; break;
        case TokenKind::SlashAssign:   op = ast::AssignOp::DivAssign; break;
        case TokenKind::PercentAssign: op = ast::AssignOp::ModAssign; break;
        default:
            return lhs;
    }
    Token op_tok = current();
    consume();
    ast::ExprPtr rhs = parse_assignment_expr();
    ast::Expr e;
    e.kind      = ast::ExprKind::Assign;
    e.range     = op_tok.range;
    e.assign_op = op;
    e.lhs       = lhs;
    e.rhs       = rhs;
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_binary_expr(int min_prec) {
    ast::ExprPtr lhs = parse_unary_expr();
    if (!lhs) return nullptr;

    while (true) {
        PrecInfo pi;
        if (!try_binary_op(pi) || pi.prec < min_prec) break;
        consume();
        int next_min = pi.right_assoc ? pi.prec : pi.prec + 1;
        ast::ExprPtr rhs = parse_binary_expr(next_min);
        ast::Expr e;
        e.kind      = ast::ExprKind::Binary;
        e.binary_op = pi.bin_op;
        e.range     = lhs->range;
        e.lhs       = lhs;
        e.rhs       = rhs;
        lhs = make(std::move(e));
    }
    return lhs;
}

ast::ExprPtr Parser::parse_unary_expr() {
    switch (current().kind) {
        case TokenKind::Minus: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind     = ast::ExprKind::Unary;
            e.unary_op = ast::UnaryOp::Neg;
            e.range    = t.range;
            e.lhs      = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::Not: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind     = ast::ExprKind::Unary;
            e.unary_op = ast::UnaryOp::Not;
            e.range    = t.range;
            e.lhs      = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::BitNot: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind     = ast::ExprKind::Unary;
            e.unary_op = ast::UnaryOp::BitNot;
            e.range    = t.range;
            e.lhs      = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::Star: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind     = ast::ExprKind::Unary;
            e.unary_op = ast::UnaryOp::Deref;
            e.range    = t.range;
            e.lhs      = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::KwMove: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind     = ast::ExprKind::Move;
            e.range    = t.range;
            e.lhs      = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::KwBorrow: {
            Token t = current();
            consume();
            bool mut = match(TokenKind::KwMut);
            ast::Expr e;
            e.kind      = ast::ExprKind::Borrow;
            e.int_value = mut ? 1 : 0;
            e.range     = t.range;
            e.lhs       = parse_unary_expr();
            return make(std::move(e));
        }
        default:
            return parse_postfix_expr();
    }
}

ast::ExprPtr Parser::parse_postfix_expr() {
    ast::ExprPtr lhs = parse_primary_expr();
    if (!lhs) return nullptr;

    while (true) {
        switch (current().kind) {
            case TokenKind::Dot: {
                Token t = current();
                consume();
                if (!check(TokenKind::Ident)) {
                    diag_.error(current().range, "expected field/method name after '.'");
                    break;
                }
                StrId name = current().str_id;
                consume();
                if (check(TokenKind::LParen)) {
                    // Method call.
                    consume();
                    ast::Expr e;
                    e.kind        = ast::ExprKind::MethodCall;
                    e.range       = t.range;
                    e.lhs         = lhs;
                    e.method_name = name;
                    while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                        e.args.push_back(parse_expr());
                        if (!match(TokenKind::Comma)) break;
                    }
                    expect(TokenKind::RParen, "')' to close method call args");
                    lhs = make(std::move(e));
                } else {
                    ast::Expr e;
                    e.kind       = ast::ExprKind::FieldAccess;
                    e.range      = t.range;
                    e.lhs        = lhs;
                    e.field_name = name;
                    lhs = make(std::move(e));
                }
                continue;
            }
            case TokenKind::LParen: {
                Token t = current();
                consume();
                ast::Expr e;
                e.kind  = ast::ExprKind::Call;
                e.range = t.range;
                e.lhs   = lhs;
                while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                    e.args.push_back(parse_expr());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen, "')' to close call args");
                lhs = make(std::move(e));
                continue;
            }
            case TokenKind::LBracket: {
                Token t = current();
                consume();
                ast::Expr e;
                e.kind  = ast::ExprKind::Index;
                e.range = t.range;
                e.lhs   = lhs;
                e.index = parse_expr();
                expect(TokenKind::RBracket, "']' to close index");
                lhs = make(std::move(e));
                continue;
            }
            case TokenKind::Question: {
                Token t = current();
                consume();
                ast::Expr e;
                e.kind  = ast::ExprKind::Question;
                e.range = t.range;
                e.lhs   = lhs;
                lhs = make(std::move(e));
                continue;
            }
            default:
                return lhs;
        }
    }
}

ast::ExprPtr Parser::parse_primary_expr() {
    switch (current().kind) {
        case TokenKind::IntLit: {
            ast::Expr e;
            e.kind      = ast::ExprKind::IntLit;
            e.range     = current().range;
            e.int_value = current().int_val;
            consume();
            return make(std::move(e));
        }
        case TokenKind::FloatLit: {
            ast::Expr e;
            e.kind        = ast::ExprKind::FloatLit;
            e.range       = current().range;
            e.float_value = current().float_val;
            consume();
            return make(std::move(e));
        }
        case TokenKind::StringLit: {
            ast::Expr e;
            e.kind      = ast::ExprKind::StringLit;
            e.range     = current().range;
            e.str_value = current().str_id;
            consume();
            return make(std::move(e));
        }
        case TokenKind::CharLit: {
            ast::Expr e;
            e.kind      = ast::ExprKind::CharLit;
            e.range     = current().range;
            e.int_value = current().int_val;
            consume();
            return make(std::move(e));
        }
        case TokenKind::BoolLit: {
            ast::Expr e;
            e.kind      = ast::ExprKind::BoolLit;
            e.range     = current().range;
            e.int_value = current().int_val;
            consume();
            return make(std::move(e));
        }
        case TokenKind::LParen: {
            Token t = current();
            consume();
            // () = unit
            if (match(TokenKind::RParen)) {
                ast::Expr e;
                e.kind  = ast::ExprKind::Tuple;
                e.range = t.range;
                return make(std::move(e));
            }
            ast::ExprPtr first = parse_expr();
            if (match(TokenKind::Comma)) {
                // Tuple.
                ast::Expr e;
                e.kind  = ast::ExprKind::Tuple;
                e.range = t.range;
                e.args.push_back(first);
                while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                    e.args.push_back(parse_expr());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen, "')' to close tuple");
                return make(std::move(e));
            }
            expect(TokenKind::RParen, "')' to close parenthesized expression");
            return first;
        }
        case TokenKind::LBracket: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind  = ast::ExprKind::ArrayLit;
            e.range = t.range;
            while (!check(TokenKind::RBracket) && !check(TokenKind::Eof)) {
                e.args.push_back(parse_expr());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RBracket, "']' to close array literal");
            return make(std::move(e));
        }
        case TokenKind::LBrace: {
            // Block expression.
            ast::Expr e;
            e.kind  = ast::ExprKind::Block;
            e.range = current().range;
            e.block = parse_block();
            return make(std::move(e));
        }
        case TokenKind::KwIf:     return parse_if_expr();
        case TokenKind::KwMatch:  return parse_match_expr();
        case TokenKind::KwLoop:   return parse_loop_expr();
        case TokenKind::KwWhile:  return parse_while_expr();
        case TokenKind::KwFor:    return parse_for_expr();
        case TokenKind::KwSpawn:  return parse_spawn_expr();
        case TokenKind::KwComptime: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind  = ast::ExprKind::Comptime;
            e.range = t.range;
            e.block = parse_block();
            return make(std::move(e));
        }
        case TokenKind::KwAlloc:  return parse_alloc_expr();
        case TokenKind::KwUnsafe: {
            ast::Expr e;
            e.kind  = ast::ExprKind::Unsafe;
            e.range = current().range;
            consume();
            e.block = parse_block();
            return make(std::move(e));
        }
        case TokenKind::KwAwait: {
            Token t = current();
            consume();
            ast::Expr e;
            e.kind  = ast::ExprKind::Await;
            e.range = t.range;
            e.lhs   = parse_unary_expr();
            return make(std::move(e));
        }
        case TokenKind::Ident:
        case TokenKind::SelfKw: {
            ast::Expr e;
            e.range = current().range;
            std::vector<StrId> path;
            path.push_back(current().str_id);
            consume();
            while (check(TokenKind::DoubleColon)) {
                consume();
                if (!check(TokenKind::Ident)) {
                    diag_.error(current().range, "expected identifier after '::'");
                    break;
                }
                path.push_back(current().str_id);
                consume();
            }
            if (path.size() == 1) {
                e.kind = ast::ExprKind::Ident;
            } else {
                e.kind = ast::ExprKind::Path;
            }
            e.path = std::move(path);
            return make(std::move(e));
        }
        default: {
            std::string msg = "expected expression, got '";
            if (const char* s = token_kind_spelling(current().kind)) msg += s;
            else msg += token_kind_name(current().kind);
            msg += "'";
            diag_.error(current().range, std::move(msg));
            // Consume the offending token so the caller makes forward
            // progress. Return a placeholder IntLit so the AST stays
            // well-formed.
            Token t = current();
            consume();
            ast::Expr e;
            e.kind  = ast::ExprKind::IntLit;
            e.range = t.range;
            e.int_value = 0;
            return make(std::move(e));
        }
    }
}

ast::ExprPtr Parser::parse_if_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::If;
    e.range = current().range;
    consume();
    e.cond        = parse_expr();
    e.then_branch = parse_block_as_expr_or_block();
    if (match(TokenKind::KwElse)) {
        if (check(TokenKind::KwIf)) {
            e.else_branch = parse_if_expr();
        } else {
            e.else_branch = parse_block_as_expr_or_block();
        }
    }
    return make(std::move(e));
}

// Helper not declared in header — inline.
ast::ExprPtr Parser::parse_block_as_expr_or_block() {
    if (check(TokenKind::LBrace)) {
        ast::Expr e;
        e.kind  = ast::ExprKind::Block;
        e.range = current().range;
        e.block = parse_block();
        return make(std::move(e));
    }
    return parse_expr();
}

ast::ExprPtr Parser::parse_match_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::Match;
    e.range = current().range;
    consume();
    e.cond = parse_expr();
    expect(TokenKind::LBrace, "'{' to open match body");
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        ast::MatchArm arm;
        arm.range = current().range;
        arm.pattern = parse_pattern();
        // Optional guard: `if guard_expr`
        if (check(TokenKind::KwIf)) {
            consume();
            arm.guard = parse_expr();
        }
        expect(TokenKind::FatArrow, "'=>' in match arm");
        arm.body = parse_expr();
        e.arms.push_back(std::move(arm));
        if (!match(TokenKind::Comma)) {
            // Allow trailing without comma if next is '}'.
            if (!check(TokenKind::RBrace)) {
                // Force recovery.
                recover_to({TokenKind::RBrace, TokenKind::Comma});
                match(TokenKind::Comma);
            }
        }
    }
    expect(TokenKind::RBrace, "'}' to close match body");
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_loop_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::Loop;
    e.range = current().range;
    consume();
    e.body = parse_block_as_expr_or_block();
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_while_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::While;
    e.range = current().range;
    consume();
    e.cond = parse_expr();
    e.body = parse_block_as_expr_or_block();
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_for_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::For;
    e.range = current().range;
    consume();
    if (!check(TokenKind::Ident)) {
        diag_.error(current().range, "expected loop variable in 'for'");
        return nullptr;
    }
    e.loop_var = current().str_id;
    consume();
    // 'in' is not a keyword. Accept any identifier whose text is "in".
    if (check(TokenKind::Ident) && intern_.get(current().str_id) == "in") {
        consume();
    } else {
        diag_.error(current().range, "expected 'in' after loop variable");
    }
    e.iterable = parse_expr();
    e.body     = parse_block_as_expr_or_block();
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_spawn_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::Spawn;
    e.range = current().range;
    consume();
    e.body = parse_block_as_expr_or_block();
    return make(std::move(e));
}

ast::ExprPtr Parser::parse_alloc_expr() {
    ast::Expr e;
    e.kind  = ast::ExprKind::Alloc;
    e.range = current().range;
    consume(); // 'alloc'

    // Syntaxes:
    //   alloc <name> = <expr>     create allocator <name> with initial value
    //   alloc <name> <expr>       allocate <expr> in allocator <name>
    //   alloc heap <expr>         allocate <expr> on the heap (special name)
    //
    // 'heap' and 'arena' are not keywords — they are just identifiers
    // that the type checker will recognize as builtin allocators.
    if (check(TokenKind::Ident)) {
        StrId first = current().str_id;
        std::string_view first_text = intern_.get(first);
        consume();

        if (check(TokenKind::Assign)) {
            // alloc <name> = <expr>
            e.alloc_target = ast::Expr::AllocTarget::Named;
            e.alloc_arena_name = first;
            consume(); // '='
            e.alloc_value = parse_expr();
            return make(std::move(e));
        }

        if (first_text == "heap") {
            e.alloc_target = ast::Expr::AllocTarget::Heap;
        } else {
            e.alloc_target = ast::Expr::AllocTarget::Named;
            e.alloc_arena_name = first;
        }
        // The value being allocated.
        if (check(TokenKind::LBrace)) {
            e.alloc_value = parse_block_as_expr_or_block();
        } else {
            e.alloc_value = parse_postfix_expr();
        }
        return make(std::move(e));
    }

    // alloc <block>   (rare, but allowed)
    if (check(TokenKind::LBrace)) {
        e.alloc_target = ast::Expr::AllocTarget::Heap;
        e.alloc_value = parse_block_as_expr_or_block();
    } else {
        e.alloc_target = ast::Expr::AllocTarget::Heap;
        e.alloc_value = parse_postfix_expr();
    }
    return make(std::move(e));
}

// ============================================================================
// Patterns
// ============================================================================

ast::PatternPtr Parser::parse_pattern() {
    ast::Pattern p;
    p.range = current().range;

    switch (current().kind) {
        case TokenKind::Ident: {
            // Could be: identifier, variant (Foo::Bar), struct.
            std::vector<StrId> path;
            path.push_back(current().str_id);
            consume();
            while (check(TokenKind::DoubleColon)) {
                consume();
                if (!check(TokenKind::Ident)) {
                    diag_.error(current().range, "expected identifier after '::' in pattern");
                    break;
                }
                path.push_back(current().str_id);
                consume();
            }
            if (path.size() == 1 && !check(TokenKind::LParen) && !check(TokenKind::LBrace)) {
                // Bare binding. (Special-case: '_' is wildcard, but it's
                // lexed as Ident currently — handle that.)
                if (intern_.get(path[0]) == "_") {
                    p.kind = ast::PatternKind::Wildcard;
                } else {
                    p.kind = ast::PatternKind::Binding;
                    p.name = path[0];
                }
                break;
            }
            // Variant or struct pattern.
            if (match(TokenKind::LParen)) {
                p.kind = ast::PatternKind::Variant;
                p.path = std::move(path);
                while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                    p.subpatterns.push_back(parse_pattern());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen, "')' to close variant pattern");
            } else if (match(TokenKind::LBrace)) {
                p.kind = ast::PatternKind::Struct;
                p.path = std::move(path);
                while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
                    ast::Pattern::FieldPattern f;
                    if (!check(TokenKind::Ident)) {
                        diag_.error(current().range, "expected field name in struct pattern");
                        break;
                    }
                    f.name = current().str_id;
                    consume();
                    if (match(TokenKind::Colon)) {
                        f.shorthand = false;
                        f.sub       = parse_pattern();
                    } else {
                        f.shorthand = true;
                    }
                    p.fields.push_back(std::move(f));
                    if (!match(TokenKind::Comma)) break;
                }
                if (check(TokenKind::DotDot)) {
                    consume();
                    p.has_rest = true;
                }
                expect(TokenKind::RBrace, "'}' to close struct pattern");
            } else {
                // Bare path pattern — treat as variant without args.
                p.kind = ast::PatternKind::Variant;
                p.path = std::move(path);
            }
            break;
        }
        case TokenKind::BoolLit: {
            p.kind       = ast::PatternKind::Bool;
            p.int_value  = current().int_val;
            consume();
            break;
        }
        case TokenKind::IntLit: {
            p.kind      = ast::PatternKind::Int;
            p.int_value = current().int_val;
            consume();
            // Check for range pattern: 1..=10 or 1..10
            if (check(TokenKind::DotDotEq)) {
                consume();
                if (!check(TokenKind::IntLit)) {
                    diag_.error(current().range, "expected integer after '..=' in range pattern");
                    break;
                }
                p.kind = ast::PatternKind::Range;
                p.int_value_hi = current().int_val;
                p.range_inclusive = true;
                consume();
            } else if (check(TokenKind::DotDot)) {
                consume();
                if (!check(TokenKind::IntLit)) {
                    diag_.error(current().range, "expected integer after '..' in range pattern");
                    break;
                }
                p.kind = ast::PatternKind::Range;
                p.int_value_hi = current().int_val;
                p.range_inclusive = false;
                consume();
            }
            break;
        }
        case TokenKind::Minus: {
            consume();
            if (!check(TokenKind::IntLit)) {
                diag_.error(current().range, "expected integer after '-' in pattern");
                break;
            }
            p.kind      = ast::PatternKind::Int;
            p.int_value = current().int_val;
            // Negate.
            p.int_value = static_cast<uint64_t>(-static_cast<int64_t>(p.int_value));
            consume();
            break;
        }
        case TokenKind::StringLit: {
            p.kind      = ast::PatternKind::String;
            p.str_value = current().str_id;
            consume();
            break;
        }
        case TokenKind::CharLit: {
            p.kind      = ast::PatternKind::Char;
            p.int_value = current().int_val;
            consume();
            break;
        }
        case TokenKind::LParen: {
            consume();
            p.kind = ast::PatternKind::Tuple;
            while (!check(TokenKind::RParen) && !check(TokenKind::Eof)) {
                p.subpatterns.push_back(parse_pattern());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen, "')' to close tuple pattern");
            break;
        }
        default: {
            std::string msg = "expected pattern, got '";
            if (const char* s = token_kind_spelling(current().kind)) msg += s;
            else msg += token_kind_name(current().kind);
            msg += "'";
            diag_.error(current().range, std::move(msg));
            p.kind = ast::PatternKind::Wildcard;
            break;
        }
    }

    // Or-pattern: pat | pat | pat
    if (check(TokenKind::Pipe)) {
        std::vector<ast::PatternPtr> alts;
        alts.push_back(make(std::move(p)));
        while (check(TokenKind::Pipe)) {
            consume();
            alts.push_back(parse_pattern());
        }
        ast::Pattern or_pat;
        or_pat.kind = ast::PatternKind::Or;
        or_pat.range = alts[0]->range;
        or_pat.alternatives = std::move(alts);
        p = std::move(or_pat);
    }

    // 'as' binding: pat as name. 'as' is not a keyword; accept
    // any ident 'as'.
    if (check(TokenKind::Ident) && intern_.get(current().str_id) == "as") {
        consume();
        if (!check(TokenKind::Ident)) {
            diag_.error(current().range, "expected binding name after 'as'");
        } else {
            ast::Pattern outer;
            outer.kind  = ast::PatternKind::As;
            outer.range = p.range;
            outer.inner = make(std::move(p));
            outer.name  = current().str_id;
            consume();
            return make(std::move(outer));
        }
    }
    return make(std::move(p));
}

} // namespace tether
