// tests/test_v7.cpp — v0.7 feature tests: or-patterns, range patterns,
// guards, where clauses, FFI calling conventions, attributes, effects

#include "test_framework.hpp"

#include "borrow/borrow.hpp"
#include "check/check.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "module/loader.hpp"
#include "parser/parser.hpp"
#include "resolve/resolve.hpp"
#include "ssa/builder.hpp"
#include "ssa/emit_llvm.hpp"
#include "ssa/mono.hpp"
#include "ssa/optimizer.hpp"
#include "ssa/rewrite.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <string>

using namespace tether;
using namespace tether::ssa;
using namespace tether::type;

struct Compiled {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    TypeContext       tc;
    Module            ssa;
    std::string       llvm_ir;

    Compiled() : tc(arena, intern) {}
};

static Compiled compile(const std::string& src) {
    Compiled c;
    uint32_t fid = c.sm.load_buffer("<test>", src);
    const SourceFile& f = c.sm.file(fid);
    Lexer lexer(c.intern, c.diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(c.intern, c.diag, c.sm, c.arena, std::move(tokens));
    auto m = parser.parse_module();

    std::vector<ast::ItemPtr> rules;
    for (ast::ItemPtr item : m->items) {
        if (item && item->kind == ast::ItemKind::Rewrite) {
            rules.push_back(item);
        }
    }
    if (!rules.empty()) {
        rewrite::Rewriter rewriter(c.diag, c.intern, c.arena);
        rewriter.apply(const_cast<ast::Module&>(*m), rules);
    }

    resolve::Resolver resolver(c.tc, c.diag, c.intern, c.arena);
    resolver.resolve_module(*m);
    check::TypeChecker checker(c.tc, c.diag, resolver, c.intern);
    checker.check_module(*m);
    mono::Monomorphizer mono(c.tc, c.diag, c.intern, c.arena);
    auto monomorphized = mono.run(*m);
    Builder builder(c.tc, c.diag, c.intern, c.arena);
    c.ssa = builder.lower_module(*monomorphized);
    Optimizer opt(c.tc, c.diag);
    opt.run(c.ssa);
    LlvmEmitter emitter(c.tc, c.diag, c.intern);
    c.llvm_ir = emitter.emit(c.ssa);
    return c;
}

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

// ---- Or-patterns ----

TETHER_TEST(or_pattern_parses) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    1 | 2 | 3 => 0,\n"
        "    _ => 1,\n"
        "  }\n"
        "}\n"));
}

// ---- Range patterns ----

TETHER_TEST(range_pattern_parses) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    1..=10 => 0,\n"
        "    11..=20 => 1,\n"
        "    _ => 2,\n"
        "  }\n"
        "}\n"));
}

TETHER_TEST(exclusive_range_pattern_parses) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    1..10 => 0,\n"
        "    _ => 1,\n"
        "  }\n"
        "}\n"));
}

// ---- Pattern guards ----

TETHER_TEST(pattern_guard_parses) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    n if n > 0 => 1,\n"
        "    n if n < 0 => 0 - 1,\n"
        "    _ => 0,\n"
        "  }\n"
        "}\n"));
}

// ---- Where clauses ----

TETHER_TEST(where_clause_parses) {
    TETHER_CHECK(parses_clean(
        "trait Ord { fn cmp(self, other: i32) -> i32 }\n"
        "fn max<T>(a: T, b: T) -> T where T: Ord {\n"
        "  return a\n"
        "}\n"));
}

TETHER_TEST(where_clause_multiple_bounds_parses) {
    TETHER_CHECK(parses_clean(
        "trait Ord { fn cmp(self, other: i32) -> i32 }\n"
        "trait Hash { fn hash(self) -> u64 }\n"
        "fn f<T>(x: T) -> T where T: Ord, T: Hash {\n"
        "  return x\n"
        "}\n"));
}

// ---- FFI calling conventions ----

TETHER_TEST(extern_c_parses) {
    TETHER_CHECK(parses_clean(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"));
}

TETHER_TEST(extern_fastcall_parses) {
    TETHER_CHECK(parses_clean(
        "extern \"fastcall\" fn fast_fn(x: i32) -> i32\n"));
}

TETHER_TEST(extern_stdcall_parses) {
    TETHER_CHECK(parses_clean(
        "extern \"stdcall\" fn std_fn(x: i32) -> i32\n"));
}

// ---- Attributes ----

TETHER_TEST(attribute_inline_parses) {
    TETHER_CHECK(parses_clean(
        "@inline fn f(x: i32) -> i32 { return x }\n"));
}

TETHER_TEST(attribute_with_args_parses) {
    TETHER_CHECK(parses_clean(
        "@inline(always) fn f(x: i32) -> i32 { return x }\n"));
}

TETHER_TEST(multiple_attributes_parses) {
    TETHER_CHECK(parses_clean(
        "@inline @cold fn f(x: i32) -> i32 { return x }\n"));
}

// ---- Effects ----

TETHER_TEST(pure_effect_parses) {
    TETHER_CHECK(parses_clean(
        "pure fn add(a: i32, b: i32) -> i32 { return a + b }\n"));
}

TETHER_TEST(io_effect_parses) {
    TETHER_CHECK(parses_clean(
        "io fn print(s: *const u8) { }\n"));
}

// ---- Macros ----

TETHER_TEST(macro_parses) {
    TETHER_CHECK(parses_clean(
        "macro Identity { x => x }\n"
        "fn f() -> i32 { return 0 }\n"));
}

TETHER_TEST(macro_with_rules_parses) {
    TETHER_CHECK(parses_clean(
        "macro Simplify {\n"
        "  x * 1 => x,\n"
        "  x * 0 => 0,\n"
        "}\n"
        "fn f() -> i32 { return 0 }\n"));
}

// ---- Existing features still work ----

TETHER_TEST(rewrite_still_works) {
    TETHER_CHECK(parses_clean(
        "rewrite Simplify { x * 1 => x }\n"
        "fn f(a: i32) -> i32 { return a * 1 }\n"));
}

TETHER_TEST(comptime_still_works) {
    TETHER_CHECK(parses_clean(
        "fn f() -> i32 { return comptime { 3 + 4 } }\n"));
}
