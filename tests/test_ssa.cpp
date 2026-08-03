// tests/test_ssa.cpp — SSA construction, optimization, and lowering tests

#include "test_framework.hpp"

#include "borrow/borrow.hpp"
#include "check/check.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "resolve/resolve.hpp"
#include "ssa/builder.hpp"
#include "ssa/emit_llvm.hpp"
#include "ssa/node.hpp"
#include "ssa/optimizer.hpp"
#include "ssa/partial_eval.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <string>

using namespace tether;
using namespace tether::ssa;
using namespace tether::type;

// Helper: compile to SSA and return all the context needed for
// rendering / lowering.
struct CompiledModule {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    TypeContext       tc;
    Module            ssa;

    CompiledModule() : tc(arena, intern) {}
};

static CompiledModule compile_to_ssa_full(const std::string& src) {
    CompiledModule c;
    uint32_t fid = c.sm.load_buffer("<test>", src);
    const SourceFile& f = c.sm.file(fid);
    Lexer lexer(c.intern, c.diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(c.intern, c.diag, c.sm, c.arena, std::move(tokens));
    auto m = parser.parse_module();
    resolve::Resolver resolver(c.tc, c.diag, c.intern, c.arena);
    resolver.resolve_module(*m);
    Builder builder(c.tc, c.diag, c.intern, c.arena);
    c.ssa = builder.lower_module(*m);
    return c;
}

static Module compile_to_ssa(const std::string& src) {
    return compile_to_ssa_full(src).ssa;
}

TETHER_TEST(ssa_empty_module) {
    auto mod = compile_to_ssa("module test");
    TETHER_CHECK_EQ(mod.functions.size(), 0u);
}

TETHER_TEST(ssa_simple_function) {
    auto mod = compile_to_ssa(
        "fn answer() -> i32 { return 42 }");
    TETHER_CHECK_EQ(mod.functions.size(), 1u);
    TETHER_CHECK_EQ(mod.functions[0].blocks.size(), 2u);  // entry + dead block
    // The first block should have a ConstInt + Ret.
    const auto& entry = mod.functions[0].blocks[0];
    TETHER_CHECK_EQ(entry.instructions.size(), 2u);
    TETHER_CHECK_EQ(entry.instructions[0].opcode, Opcode::ConstInt);
    TETHER_CHECK_EQ(entry.instructions[1].opcode, Opcode::Ret);
}

TETHER_TEST(ssa_arithmetic) {
    auto mod = compile_to_ssa(
        "fn add(a: i32, b: i32) -> i32 { return a + b }");
    TETHER_CHECK_EQ(mod.functions.size(), 1u);
    const auto& entry = mod.functions[0].blocks[0];
    // Should have: ConstInt? No — params + Add + Ret.
    // Actually params don't produce instructions. So: Add + Ret.
    TETHER_CHECK_EQ(entry.instructions.size(), 2u);
    TETHER_CHECK_EQ(entry.instructions[0].opcode, Opcode::Add);
    TETHER_CHECK_EQ(entry.instructions[1].opcode, Opcode::Ret);
}

TETHER_TEST(ssa_call_emitted) {
    auto mod = compile_to_ssa(
        "fn f() -> i32 { return g(1) }"
        "fn g(x: i32) -> i32 { return x }");
    TETHER_CHECK_EQ(mod.functions.size(), 2u);
    // Find f (the one with a Call).
    const Function* f = nullptr;
    for (const auto& fn : mod.functions) {
        if (!fn.blocks.empty() && !fn.blocks[0].instructions.empty()) {
            for (const auto& inst : fn.blocks[0].instructions) {
                if (inst.opcode == Opcode::Call) {
                    f = &fn;
                    break;
                }
            }
        }
    }
    TETHER_CHECK(f != nullptr);
}

TETHER_TEST(ssa_constant_folding) {
    auto c = compile_to_ssa_full(
        "fn f() -> i32 { return 1 + 2 }");
    Optimizer opt(c.tc, c.diag);
    uint32_t changes = opt.run(c.ssa);
    TETHER_CHECK(changes > 0);
    // After folding, the Add should become a ConstInt.
    const auto& entry = c.ssa.functions[0].blocks[0];
    TETHER_CHECK_EQ(entry.instructions[0].opcode, Opcode::ConstInt);
}

TETHER_TEST(ssa_emits_llvm_ir) {
    auto c = compile_to_ssa_full(
        "fn add(a: i32, b: i32) -> i32 { return a + b }");
    LlvmEmitter emitter(c.tc, c.diag, c.intern);
    std::string ir = emitter.emit(c.ssa);
    TETHER_CHECK(ir.find("define") != std::string::npos);
    TETHER_CHECK(ir.find("@_tether_add") != std::string::npos);
    TETHER_CHECK(ir.find("add i64") != std::string::npos);
}

TETHER_TEST(ssa_renders_module) {
    auto c = compile_to_ssa_full(
        "fn f() -> i32 { return 0 }");
    std::string text = render_module(c.ssa, c.tc);
    TETHER_CHECK(text.find("fn ") != std::string::npos);
    TETHER_CHECK(text.find("const-int") != std::string::npos);
    TETHER_CHECK(text.find("ret") != std::string::npos);
}

TETHER_TEST(partial_eval_simple) {
    auto c = compile_to_ssa_full(
        "fn seven() -> i32 { return 3 + 4 }");
    Optimizer opt(c.tc, c.diag);
    opt.run(c.ssa);  // fold 3 + 4 → 7
    PartialEvaluator eval(c.tc, c.diag);
    auto result = eval.evaluate(c.ssa.functions[0], {});
    TETHER_CHECK(result.has_value());
    TETHER_CHECK_EQ(*result, 7u);
}

TETHER_TEST(partial_eval_with_args) {
    auto c = compile_to_ssa_full(
        "fn add(a: i32, b: i32) -> i32 { return a + b }");
    PartialEvaluator eval(c.tc, c.diag);
    auto result = eval.evaluate(c.ssa.functions[0], {10, 20});
    TETHER_CHECK(result.has_value());
    TETHER_CHECK_EQ(*result, 30u);
}
