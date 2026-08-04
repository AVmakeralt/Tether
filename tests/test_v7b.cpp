// tests/test_v7b.cpp — const generics, GADTs, reflection tests

#include "test_framework.hpp"

#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

using namespace tether;

static bool parses_clean(const std::string& src) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    uint32_t fid = sm.load_buffer("<test>", src);
    const SourceFile& f = sm.file(fid);
    Lexer lexer(intern, diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern, diag, sm, arena, std::move(tokens));
    auto m = parser.parse_module();
    return !diag.has_errors();
}

// ---- Const generics ----

TETHER_TEST(const_generic_param_parses) {
    TETHER_CHECK(parses_clean(
        "fn f<const N>(arr: [i32; N]) -> i32 {\n"
        "  return arr[0]\n"
        "}\n"));
}

TETHER_TEST(const_generic_with_type_param_parses) {
    TETHER_CHECK(parses_clean(
        "fn map<T, const N>(arr: [T; N]) -> i32 {\n"
        "  return 0\n"
        "}\n"));
}

// ---- GADTs ----

TETHER_TEST(gadt_enum_parses) {
    TETHER_CHECK(parses_clean(
        "enum Expr<T> {\n"
        "  Int(i64) -> Expr<i64>,\n"
        "  Bool(bool) -> Expr<bool>,\n"
        "}\n"));
}

TETHER_TEST(gadt_mixed_with_regular_variants_parses) {
    TETHER_CHECK(parses_clean(
        "enum Expr<T> {\n"
        "  Int(i64) -> Expr<i64>,\n"
        "  Bool(bool) -> Expr<bool>,\n"
        "  Var(u64),\n"
        "}\n"));
}

// ---- Compile-time reflection ----

TETHER_TEST(reflect_parses) {
    TETHER_CHECK(parses_clean(
        "fn f() -> i32 {\n"
        "  let s = reflect(i32)\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(reflect_with_struct_parses) {
    TETHER_CHECK(parses_clean(
        "struct Point { x: i32, y: i32 }\n"
        "fn f() -> i32 {\n"
        "  let info = reflect(Point)\n"
        "  return 0\n"
        "}\n"));
}

// ---- Combined features ----

TETHER_TEST(const_generic_with_where_parses) {
    TETHER_CHECK(parses_clean(
        "trait Clone { fn clone(self) -> i32 }\n"
        "fn copy<T, const N>(arr: [T; N]) -> i32 where T: Clone {\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(gadt_with_match_parses) {
    TETHER_CHECK(parses_clean(
        "enum Expr<T> {\n"
        "  Int(i64) -> Expr<i64>,\n"
        "  Bool(bool) -> Expr<bool>,\n"
        "}\n"
        "fn eval(e: Expr<i64>) -> i64 {\n"
        "  match e {\n"
        "    Int(n) => n,\n"
        "    _ => 0,\n"
        "  }\n"
        "}\n"));
}

// ---- Existing features still work ----

TETHER_TEST(or_pattern_still_works) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x { 1 | 2 | 3 => 0, _ => 1 }\n"
        "}\n"));
}

TETHER_TEST(range_pattern_still_works) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x { 1..=10 => 0, _ => 1 }\n"
        "}\n"));
}

TETHER_TEST(pattern_guard_still_works) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x { n if n > 0 => 1, _ => 0 }\n"
        "}\n"));
}

TETHER_TEST(extern_c_still_works) {
    TETHER_CHECK(parses_clean(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"));
}

TETHER_TEST(attributes_still_works) {
    TETHER_CHECK(parses_clean(
        "@inline(always) fn f(x: i32) -> i32 { return x }\n"));
}

TETHER_TEST(where_clause_still_works) {
    TETHER_CHECK(parses_clean(
        "trait Ord { fn cmp(self) -> i32 }\n"
        "fn max<T>(a: T, b: T) -> T where T: Ord { return a }\n"));
}

TETHER_TEST(pure_effect_still_works) {
    TETHER_CHECK(parses_clean(
        "pure fn add(a: i32, b: i32) -> i32 { return a + b }\n"));
}

TETHER_TEST(macro_still_works) {
    TETHER_CHECK(parses_clean(
        "macro Id { x => x }\n"
        "fn f() -> i32 { return 0 }\n"));
}
