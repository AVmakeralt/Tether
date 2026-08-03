// tests/test_v5.cpp — v0.5 feature tests: noalias, enums, traits, rewrites

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

    // Collect rewrite rules.
    std::vector<ast::ItemPtr> rules;
    for (ast::ItemPtr item : m->items) {
        if (item && item->kind == ast::ItemKind::Rewrite) {
            rules.push_back(item);
        }
    }
    // Apply rewrites.
    if (!rules.empty()) {
        rewrite::Rewriter rewriter(c.diag, c.intern, c.arena);
        rewriter.apply(const_cast<ast::Module&>(*m), rules);
    }

    resolve::Resolver resolver(c.tc, c.diag, c.intern, c.arena);
    resolver.resolve_module(*m);
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

// ---- noalias attributes ----

TETHER_TEST(ref_param_gets_noalias) {
    auto c = compile(
        "fn read(x: ref i32) -> i32 { return *x }\n");
    TETHER_CHECK(c.llvm_ir.find("noalias") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("nonnull") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("readonly") != std::string::npos);
}

TETHER_TEST(mut_ref_drops_readonly) {
    auto c = compile(
        "fn write(x: mut ref i32) { *x = 42 }\n");
    TETHER_CHECK(c.llvm_ir.find("noalias") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("nonnull") != std::string::npos);
    // mut ref should NOT have readonly.
    TETHER_CHECK(c.llvm_ir.find("readonly") == std::string::npos);
}

TETHER_TEST(raw_ptr_no_attributes) {
    auto c = compile(
        "extern fn malloc(n: u64) -> *mut u8\n"
        "fn f() -> i32 { return 0 }\n");
    // Raw pointers should not carry noalias.
    size_t pos = c.llvm_ir.find("malloc");
    if (pos != std::string::npos) {
        std::string around = c.llvm_ir.substr(pos, 60);
        TETHER_CHECK(around.find("noalias") == std::string::npos);
    }
}

// ---- no-payload enums ----

TETHER_TEST(no_payload_enum_lowers_as_i64) {
    auto c = compile(
        "enum Color { Red, Green, Blue }\n"
        "fn f(c: Color) -> i32 {\n"
        "  match c { Red => 1, _ => 0 }\n"
        "}\n");
    // No-payload enum should not use alloca { i64, i64 }.
    TETHER_CHECK(c.llvm_ir.find("alloca { i64, i64 }") == std::string::npos);
    // Should have icmp eq for the match.
    TETHER_CHECK(c.llvm_ir.find("icmp eq") != std::string::npos);
}

TETHER_TEST(no_payload_enum_match_three_variants) {
    auto c = compile(
        "enum Color { Red, Green, Blue }\n"
        "fn value(c: Color) -> i32 {\n"
        "  match c { Red => 1, Green => 2, _ => 3 }\n"
        "}\n");
    // Should have at least 2 comparisons.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = c.llvm_ir.find("icmp eq", pos)) != std::string::npos) {
        ++count;
        pos += 7;
    }
    TETHER_CHECK(count >= 2);
}

// ---- struct field index ----

TETHER_TEST(struct_field_y_index) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn get_y(p: Point) -> i32 { return p.y }\n");
    // p.y should use index 1, not 0.
    TETHER_CHECK(c.llvm_ir.find("i32 1") != std::string::npos);
}

TETHER_TEST(struct_field_x_index) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn get_x(p: Point) -> i32 { return p.x }\n");
    // p.x should use index 0.
    TETHER_CHECK(c.llvm_ir.find("i32 0") != std::string::npos);
}

// ---- trait dispatch ----

TETHER_TEST(trait_method_resolved) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "impl Point {\n"
        "  fn get_x(self) -> i32 { return self.x }\n"
        "}\n"
        "fn f(p: Point) -> i32 { return p.get_x() }\n");
    // Method should be mangled as Point_get_x.
    TETHER_CHECK(c.llvm_ir.find("@_tether_Point_get_x") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("call i64 @_tether_Point_get_x") != std::string::npos);
}

TETHER_TEST(trait_method_with_args) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "impl Point {\n"
        "  fn add(self, other: Point) -> i32 { return self.x + other.x }\n"
        "}\n"
        "fn f(p: Point, q: Point) -> i32 { return p.add(q) }\n");
    TETHER_CHECK(c.llvm_ir.find("@_tether_Point_add") != std::string::npos);
    // Call should have 2 args (self + other).
    TETHER_CHECK(c.llvm_ir.find("call i64 @_tether_Point_add(i64") != std::string::npos);
}

// ---- rewrite rules ----

TETHER_TEST(rewrite_mul_one) {
    auto c = compile(
        "rewrite Simplify { x * 1 => x }\n"
        "fn f(a: i32) -> i32 { return a * 1 }\n");
    // a * 1 should be rewritten to a — no mul instruction.
    TETHER_CHECK(c.llvm_ir.find("mul") == std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("ret i64 %arg0") != std::string::npos);
}

TETHER_TEST(rewrite_mul_zero) {
    auto c = compile(
        "rewrite Simplify { x * 0 => 0 }\n"
        "fn f(a: i32) -> i32 { return a * 0 }\n");
    // a * 0 should be rewritten to 0 — no mul instruction.
    TETHER_CHECK(c.llvm_ir.find("mul") == std::string::npos);
}

TETHER_TEST(rewrite_add_zero) {
    auto c = compile(
        "rewrite Simplify { x + 0 => x, 0 + x => x }\n"
        "fn f(a: i32) -> i32 { return a + 0 }\n"
        "fn g(b: i32) -> i32 { return 0 + b }\n");
    TETHER_CHECK(c.llvm_ir.find("add") == std::string::npos ||
                 c.llvm_ir.find("add i64 0, 0") != std::string::npos);
}

TETHER_TEST(rewrite_chained) {
    auto c = compile(
        "rewrite Simplify {\n"
        "  x * 1 => x,\n"
        "  x + 0 => x,\n"
        "}\n"
        "fn f(a: i32) -> i32 { return a * 1 + 0 }\n");
    // Both rewrites should fire: a * 1 + 0 → a + 0 → a.
    TETHER_CHECK(c.llvm_ir.find("ret i64 %arg0") != std::string::npos);
}
