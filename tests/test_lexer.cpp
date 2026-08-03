// tests/test_lexer.cpp — lexer tests

#include "test_framework.hpp"

#include "lexer/lexer.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

using namespace tether;

static std::vector<Token> lex(const std::string& src) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    uint32_t          fid = sm.load_buffer("<test>", src);
    const SourceFile& f   = sm.file(fid);
    Lexer             lexer(intern, diag, f);
    return lexer.tokenize();
}

TETHER_TEST(lex_empty) {
    auto toks = lex("");
    TETHER_CHECK_EQ(toks.back().kind, TokenKind::Eof);
}

TETHER_TEST(lex_whitespace_and_comments) {
    auto toks = lex("// line comment\n /* block */  \n");
    TETHER_CHECK_EQ(toks.back().kind, TokenKind::Eof);
    TETHER_CHECK_EQ(toks.size(), 1u);
}

TETHER_TEST(lex_nested_block_comment) {
    auto toks = lex("/* outer /* inner */ still outer */");
    TETHER_CHECK_EQ(toks.back().kind, TokenKind::Eof);
    TETHER_CHECK_EQ(toks.size(), 1u);
}

TETHER_TEST(lex_all_34_keywords) {
    // The 34 keywords + 'self' (contextual).
    const char* src =
        "module import export "
        "fn struct enum union trait impl type alias "
        "let mut const static "
        "if else match while for loop break continue return defer "
        "alloc move borrow unsafe "
        "extern ffi comptime "
        "spawn await self";
    auto toks = lex(src);
    // 35 keyword tokens + EOF.
    TETHER_CHECK_EQ(toks.size(), 36u);

    // Sanity check a few.
    TETHER_CHECK_EQ(toks[0].kind,  TokenKind::KwModule);
    TETHER_CHECK_EQ(toks[1].kind,  TokenKind::KwImport);
    TETHER_CHECK_EQ(toks[2].kind,  TokenKind::KwExport);
    TETHER_CHECK_EQ(toks[3].kind,  TokenKind::KwFn);
    TETHER_CHECK_EQ(toks[33].kind, TokenKind::KwAwait);
    TETHER_CHECK_EQ(toks[34].kind, TokenKind::SelfKw);
    TETHER_CHECK_EQ(toks[35].kind, TokenKind::Eof);
}

TETHER_TEST(lex_bool_literals) {
    auto toks = lex("true false");
    TETHER_CHECK_EQ(toks[0].kind, TokenKind::BoolLit);
    TETHER_CHECK_EQ(toks[0].int_val, 1u);
    TETHER_CHECK_EQ(toks[1].kind, TokenKind::BoolLit);
    TETHER_CHECK_EQ(toks[1].int_val, 0u);
}

TETHER_TEST(lex_decimal_int) {
    auto toks = lex("42 1_000_000");
    TETHER_CHECK_EQ(toks[0].kind, TokenKind::IntLit);
    TETHER_CHECK_EQ(toks[0].int_val, 42u);
    TETHER_CHECK_EQ(toks[1].kind, TokenKind::IntLit);
    TETHER_CHECK_EQ(toks[1].int_val, 1000000u);
}

TETHER_TEST(lex_hex_bin_oct) {
    auto toks = lex("0xFF 0b1010 0o777");
    TETHER_CHECK_EQ(toks[0].int_val, 0xFFu);
    TETHER_CHECK_EQ(toks[1].int_val, 10u);
    TETHER_CHECK_EQ(toks[2].int_val, 0777u);
}

TETHER_TEST(lex_float) {
    auto toks = lex("3.14 1e10 1.5e-3");
    TETHER_CHECK_EQ(toks[0].kind, TokenKind::FloatLit);
    TETHER_CHECK_EQ(toks[1].kind, TokenKind::FloatLit);
    TETHER_CHECK_EQ(toks[2].kind, TokenKind::FloatLit);
}

TETHER_TEST(lex_string_escapes) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    uint32_t          fid = sm.load_buffer("<test>", std::string("\"a\\nb\\tc\""));
    const SourceFile& f   = sm.file(fid);
    Lexer             lexer(intern, diag, f);
    auto toks = lexer.tokenize();
    TETHER_CHECK_EQ(toks[0].kind, TokenKind::StringLit);
    std::string_view s = intern.get(toks[0].str_id);
    TETHER_CHECK_EQ(s, std::string_view("a\nb\tc"));
}

TETHER_TEST(lex_operators) {
    auto toks = lex("+ - * / % == != < > <= >= && || ! & | ^ ~ << >> "
                    "-> => :: = += -= *= /= %= ? : . , ; ( ) { } [ ] @ #");
    // Just spot-check a few. Indices:
    //   0:+  1:-  2:*  3:/  4:%  5:==  6:!=  7:<  8:>  9:<=  10:>=
    //  11:&& 12:|| 13:!  14:&  15:|  16:^  17:~  18:<< 19:>> 20:->
    //  21:=> 22:::  23:=  24:+= ...
    TETHER_CHECK_EQ(toks[0].kind,  TokenKind::Plus);
    TETHER_CHECK_EQ(toks[5].kind,  TokenKind::Eq);
    TETHER_CHECK_EQ(toks[6].kind,  TokenKind::Neq);
    TETHER_CHECK_EQ(toks[20].kind, TokenKind::Arrow);
    TETHER_CHECK_EQ(toks[21].kind, TokenKind::FatArrow);
    TETHER_CHECK_EQ(toks[22].kind, TokenKind::DoubleColon);
}

TETHER_TEST(lex_identifiers) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    uint32_t          fid = sm.load_buffer("<test>", std::string("foo bar_baz Qux"));
    const SourceFile& f   = sm.file(fid);
    Lexer             lexer(intern, diag, f);
    auto toks = lexer.tokenize();
    TETHER_CHECK_EQ(toks[0].kind, TokenKind::Ident);
    TETHER_CHECK_EQ(intern.get(toks[0].str_id), std::string_view("foo"));
    TETHER_CHECK_EQ(toks[1].kind, TokenKind::Ident);
    TETHER_CHECK_EQ(intern.get(toks[1].str_id), std::string_view("bar_baz"));
    TETHER_CHECK_EQ(toks[2].kind, TokenKind::Ident);
    TETHER_CHECK_EQ(intern.get(toks[2].str_id), std::string_view("Qux"));
}
