// tests/test_v6.cpp — v0.6 feature tests: type inference, phi nodes,
// comptime, trait bounds

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

static Compiled compile_no_opt(const std::string& src) {
    Compiled c;
    uint32_t fid = c.sm.load_buffer("<test>", src);
    const SourceFile& f = c.sm.file(fid);
    Lexer lexer(c.intern, c.diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(c.intern, c.diag, c.sm, c.arena, std::move(tokens));
    auto m = parser.parse_module();
    resolve::Resolver resolver(c.tc, c.diag, c.intern, c.arena);
    resolver.resolve_module(*m);
    mono::Monomorphizer mono(c.tc, c.diag, c.intern, c.arena);
    auto monomorphized = mono.run(*m);
    Builder builder(c.tc, c.diag, c.intern, c.arena);
    c.ssa = builder.lower_module(*monomorphized);
    LlvmEmitter emitter(c.tc, c.diag, c.intern);
    c.llvm_ir = emitter.emit(c.ssa);
    return c;
}

// ---- Generic type inference ----

TETHER_TEST(generic_infer_int) {
    auto c = compile_no_opt(
        "fn id<T>(x: T) -> T { return x }\n"
        "fn f() -> i32 { return id(42) }\n");
    // id(42) should create id_i64 (int literal → i64).
    TETHER_CHECK(c.llvm_ir.find("@_tether_id_i64") != std::string::npos);
}

TETHER_TEST(generic_infer_bool) {
    auto c = compile_no_opt(
        "fn id<T>(x: T) -> T { return x }\n"
        "fn f() -> i32 { return id(true) }\n");
    // id(true) should create id_bool.
    TETHER_CHECK(c.llvm_ir.find("@_tether_id_bool") != std::string::npos);
}

TETHER_TEST(generic_infer_multiple_types) {
    auto c = compile_no_opt(
        "fn id<T>(x: T) -> T { return x }\n"
        "fn f() -> i32 { return id(42) + id(true) }\n");
    // Both id_i64 and id_bool should be created.
    TETHER_CHECK(c.llvm_ir.find("@_tether_id_i64") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("@_tether_id_bool") != std::string::npos);
}

TETHER_TEST(generic_two_params) {
    auto c = compile_no_opt(
        "fn add<T>(a: T, b: T) -> T { return a + b }\n"
        "fn f() -> i32 { return add(10, 20) }\n");
    TETHER_CHECK(c.llvm_ir.find("@_tether_add_i64") != std::string::npos);
}

// ---- Phi nodes ----

TETHER_TEST(if_else_phi_merges_values) {
    auto c = compile_no_opt(
        "fn abs(x: i32) -> i32 {\n"
        "  return if x < 0 { 0 - x } else { x }\n"
        "}\n");
    TETHER_CHECK(c.llvm_ir.find("phi i64") != std::string::npos);
    // Phi should have two incoming values.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = c.llvm_ir.find("[ %", pos)) != std::string::npos) {
        ++count;
        pos += 3;
    }
    TETHER_CHECK(count >= 2);
}

TETHER_TEST(match_phi_merges_arms) {
    auto c = compile_no_opt(
        "enum Color { Red(i32), Green(i32), Blue(i32) }\n"
        "fn value(c: Color) -> i32 {\n"
        "  return match c {\n"
        "    Red(r)   => r,\n"
        "    Green(g) => g,\n"
        "    Blue(b)  => b,\n"
        "  }\n"
        "}\n");
    TETHER_CHECK(c.llvm_ir.find("phi i64") != std::string::npos);
}

TETHER_TEST(if_without_else_no_phi) {
    auto c = compile_no_opt(
        "fn f(x: i32) {\n"
        "  if x > 0 { return 1 }\n"
        "}\n");
    // No phi needed — the if has no else and doesn't produce a value.
    TETHER_CHECK(c.llvm_ir.find("phi") == std::string::npos);
}

// ---- Comptime ----

TETHER_TEST(comptime_evaluates_at_compile_time) {
    auto c = compile(
        "fn seven() -> i32 {\n"
        "  return comptime { 3 + 4 }\n"
        "}\n");
    // 3 + 4 should be folded to 7.
    TETHER_CHECK(c.llvm_ir.find("add i64 7, 0") != std::string::npos);
    // No runtime add of 3 + 4 should remain.
    TETHER_CHECK(c.llvm_ir.find("add i64 3, 4") == std::string::npos);
}

TETHER_TEST(comptime_complex_expression) {
    auto c = compile(
        "fn answer() -> i32 {\n"
        "  return comptime { (2 + 3) * 4 }\n"
        "}\n");
    // (2 + 3) * 4 = 20.
    TETHER_CHECK(c.llvm_ir.find("add i64 20, 0") != std::string::npos);
}

TETHER_TEST(comptime_in_function) {
    auto c = compile(
        "fn f() -> i32 {\n"
        "  let x = comptime { 10 * 10 }\n"
        "  return x\n"
        "}\n");
    // 10 * 10 = 100.
    TETHER_CHECK(c.llvm_ir.find("add i64 100, 0") != std::string::npos);
}

// ---- Trait bounds checking ----

TETHER_TEST(trait_impl_satisfies_trait) {
    auto c = compile(
        "trait Hash { fn hash(self) -> u64 }\n"
        "struct Point { x: i32 }\n"
        "impl Hash for Point {\n"
        "  fn hash(self) -> u64 { return 42 }\n"
        "}\n"
        "fn f() -> i32 { return 0 }\n");
    // No errors — impl provides the required method.
    TETHER_CHECK(!c.diag.has_errors());
}

TETHER_TEST(trait_impl_missing_method) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    TypeContext       tc(arena, intern);

    uint32_t fid = sm.load_buffer("<test>",
        "trait Hash { fn hash(self) -> u64 }\n"
        "struct Point { x: i32 }\n"
        "impl Hash for Point {\n"
        "  fn wrong_name(self) -> u64 { return 42 }\n"
        "}\n");
    const SourceFile& f = sm.file(fid);
    Lexer lexer(intern, diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern, diag, sm, arena, std::move(tokens));
    auto m = parser.parse_module();
    resolve::Resolver resolver(tc, diag, intern, arena);
    resolver.resolve_module(*m);
    check::TypeChecker checker(tc, diag, resolver, intern);
    checker.check_module(*m);
    TETHER_CHECK(diag.has_errors());
    std::string rendered = diag.render(sm);
    TETHER_CHECK(rendered.find("missing method 'hash'") != std::string::npos);
}

TETHER_TEST(trait_impl_wrong_param_count) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    TypeContext       tc(arena, intern);

    uint32_t fid = sm.load_buffer("<test>",
        "trait Hash { fn hash(self) -> u64 }\n"
        "struct Point { x: i32 }\n"
        "impl Hash for Point {\n"
        "  fn hash(self, extra: i32) -> u64 { return 42 }\n"
        "}\n");
    const SourceFile& f = sm.file(fid);
    Lexer lexer(intern, diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern, diag, sm, arena, std::move(tokens));
    auto m = parser.parse_module();
    resolve::Resolver resolver(tc, diag, intern, arena);
    resolver.resolve_module(*m);
    check::TypeChecker checker(tc, diag, resolver, intern);
    checker.check_module(*m);
    TETHER_CHECK(diag.has_errors());
    std::string rendered = diag.render(sm);
    TETHER_CHECK(rendered.find("wrong parameter count") != std::string::npos);
}

TETHER_TEST(trait_impl_missing_return_type) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    TypeContext       tc(arena, intern);

    uint32_t fid = sm.load_buffer("<test>",
        "trait Hash { fn hash(self) -> u64 }\n"
        "struct Point { x: i32 }\n"
        "impl Hash for Point {\n"
        "  fn hash(self) { return }\n"
        "}\n");
    const SourceFile& f = sm.file(fid);
    Lexer lexer(intern, diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern, diag, sm, arena, std::move(tokens));
    auto m = parser.parse_module();
    resolve::Resolver resolver(tc, diag, intern, arena);
    resolver.resolve_module(*m);
    check::TypeChecker checker(tc, diag, resolver, intern);
    checker.check_module(*m);
    TETHER_CHECK(diag.has_errors());
    std::string rendered = diag.render(sm);
    TETHER_CHECK(rendered.find("missing required return type") != std::string::npos);
}

TETHER_TEST(trait_without_impl_no_error) {
    // Defining a trait without any impl should not error.
    auto c = compile(
        "trait Drawable { fn draw(self) }\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(!c.diag.has_errors());
}
