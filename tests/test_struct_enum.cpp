// tests/test_struct_enum.cpp — struct/enum/match tests

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

TETHER_TEST(struct_construction) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn make(a: i32, b: i32) -> Point { return Point(a, b) }\n");
    TETHER_CHECK(c.llvm_ir.find("alloca { i64, i64 }") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("getelementptr") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("store i64") != std::string::npos);
}

TETHER_TEST(struct_field_access) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn get_x(p: Point) -> i32 { return p.x }\n");
    TETHER_CHECK(c.llvm_ir.find("getelementptr") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("load i64") != std::string::npos);
}

TETHER_TEST(enum_construction) {
    auto c = compile(
        "enum Shape { Circle(i32), Square(i32) }\n"
        "fn make_circle(r: i32) -> Shape { return Circle(r) }\n");
    TETHER_CHECK(c.llvm_ir.find("alloca { i64, i64 }") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("store i64 0") != std::string::npos);
}

TETHER_TEST(enum_match_lowers_to_branches) {
    auto c = compile(
        "enum Shape { Circle(i32), Square(i32) }\n"
        "fn area(s: Shape) -> i32 {\n"
        "  match s {\n"
        "    Circle(r) => r,\n"
        "    Square(s) => s,\n"
        "  }\n"
        "}\n");
    // Match should lower to tag extraction + comparisons + branches.
    TETHER_CHECK(c.llvm_ir.find("icmp eq") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("br i1") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("label %bb") != std::string::npos);
}

TETHER_TEST(enum_match_three_variants) {
    auto c = compile(
        "enum Color { Red(i32), Green(i32), Blue(i32) }\n"
        "fn value(c: Color) -> i32 {\n"
        "  match c {\n"
        "    Red(r)   => r,\n"
        "    Green(g) => g,\n"
        "    Blue(b)  => b,\n"
        "  }\n"
        "}\n");
    // Three variants → at least two comparisons (the third is the
    // fallthrough / else branch).
    size_t count = 0;
    size_t pos = 0;
    while ((pos = c.llvm_ir.find("icmp eq", pos)) != std::string::npos) {
        ++count;
        pos += 7;
    }
    TETHER_CHECK(count >= 2);
}

TETHER_TEST(generic_fn_monomorphized) {
    auto c = compile(
        "fn max<T: Ord>(a: T, b: T) -> T {\n"
        "  if a > b { return a } else { return b }\n"
        "}\n"
        "fn f() -> i32 { return max(3, 7) }\n");
    // The generic fn should be present (mangled or not).
    TETHER_CHECK(c.llvm_ir.find("@_tether_max") != std::string::npos);
    // The call should reference the instantiated name.
    TETHER_CHECK(c.llvm_ir.find("call i64 @_tether_max") != std::string::npos);
}

TETHER_TEST(generic_fn_two_call_sites) {
    auto c = compile(
        "fn id<T>(x: T) -> T { return x }\n"
        "fn f() -> i32 { return id(1) + id(2) }\n");
    // Both calls should go to the same instantiated function.
    size_t call_count = 0;
    size_t pos = 0;
    while ((pos = c.llvm_ir.find("call i64 @_tether_id", pos)) != std::string::npos) {
        ++call_count;
        pos += 5;
    }
    TETHER_CHECK(call_count >= 2);
}

TETHER_TEST(if_else_proper_cfg) {
    auto c = compile(
        "fn abs(x: i32) -> i32 {\n"
        "  if x < 0 { return 0 - x } else { return x }\n"
        "}\n");
    TETHER_CHECK(c.llvm_ir.find("br i1") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("label %bb") != std::string::npos);
}

TETHER_TEST(while_loop_proper_cfg) {
    auto c = compile(
        "fn count() {\n"
        "  let i = 0\n"
        "  while i < 10 { i = i + 1 }\n"
        "}\n");
    // The while loop should produce branch instructions.
    TETHER_CHECK(c.llvm_ir.find("br label") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("br i1") != std::string::npos);
}
