// tests/test_ffi.cpp — v0.9 zero-overhead FFI tests
//
// These tests verify the design documented in docs/ssa-ir.md and the
// architecture discussion: Tether's FFI is a type/ABI lowering system,
// not a runtime wrapper. Concretely:
//
//   - extern "C" functions are referenced by their bare symbol name
//     (no `_tether_` mangling), so a Tether call to `extern fn printf`
//     lowers to `call i32 (i8*, ...) @printf(...)` with no wrapper.
//   - Calling conventions propagate from `extern "C"` / `extern "fastcall"`
//     / etc. to LLVM's `declare`/`call` instructions.
//   - Function signatures use the declared types (i32 stays i32, not i64).
//   - Calls look up the callee's signature and emit matching arg/return
//     types, inserting sext/zext/trunc as needed.
//   - Struct construction uses the real struct layout (`{ i32, double }`,
//     not the v0.2–v0.8 `{ i64, i64 }` widening).
//   - ref T and *const T both lower to LLVM pointers, with refs carrying
//     noalias/nonnull/readonly attributes and raw pointers carrying none.

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

// ---- Extern name mangling -------------------------------------------
//
// The most important property of the v0.9 FFI: extern functions are
// referenced by their bare C symbol. A Tether call to `extern fn
// printf` lowers to `call ... @printf(...)`, NOT `call ...
// @_tether_printf(...)`. This is what makes the FFI zero-overhead —
// the linker resolves the call directly to the C library function,
// with no wrapper, no trampoline, no conversion.

TETHER_TEST(extern_decl_uses_bare_name) {
    auto c = compile(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    // The declare must use @printf, not @_tether_printf.
    TETHER_CHECK(c.llvm_ir.find("declare i32 @printf(") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("@_tether_printf") == std::string::npos);
}

TETHER_TEST(extern_call_uses_bare_name) {
    auto c = compile(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"
        "fn f() -> i32 {\n"
        "  printf(\"hello\")\n"
        "  return 0\n"
        "}\n");
    // The call must reference @printf directly.
    TETHER_CHECK(c.llvm_ir.find("call i32 @printf(") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("@_tether_printf") == std::string::npos);
}

TETHER_TEST(tether_fn_uses_mangled_name) {
    auto c = compile(
        "fn helper(x: i32) -> i32 { return x }\n"
        "fn f() -> i32 { return helper(42) }\n");
    // Tether-defined functions get the _tether_ prefix.
    TETHER_CHECK(c.llvm_ir.find("define i32 @_tether_helper(")
                 != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("call i32 @_tether_helper(")
                 != std::string::npos);
}

// ---- Calling conventions --------------------------------------------
//
// Each `extern "X"` keyword maps to an LLVM calling-convention token
// on the `declare` and the `call`. The default `ccc` is omitted in
// text IR (it's LLVM's default), so `extern "C"` produces no token;
// `extern "fastcall"` produces `fastcc`; etc.

TETHER_TEST(extern_c_emits_no_cc_token) {
    auto c = compile(
        "extern \"C\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    // ccc is the default — no token emitted. The declare should be
    // `declare i32 @foo(i32)`, not `declare ccc i32 @foo(i32)`.
    TETHER_CHECK(c.llvm_ir.find("declare i32 @foo(") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("ccc ") == std::string::npos);
}

TETHER_TEST(extern_fastcall_emits_fastcc) {
    auto c = compile(
        "extern \"fastcall\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return foo(1) }\n");
    TETHER_CHECK(c.llvm_ir.find("declare fastcc i32 @foo(")
                 != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("call fastcc i32 @foo(")
                 != std::string::npos);
}

TETHER_TEST(extern_stdcall_emits_x86_stdcallcc) {
    auto c = compile(
        "extern \"stdcall\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare x86_stdcallcc i32 @foo(")
                 != std::string::npos);
}

TETHER_TEST(extern_vectorcall_emits_x86_vectorcallcc) {
    auto c = compile(
        "extern \"vectorcall\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare x86_vectorcallcc i32 @foo(")
                 != std::string::npos);
}

TETHER_TEST(extern_sysv_emits_x86_64_sysvcc) {
    auto c = compile(
        "extern \"sysv\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare x86_64_sysvcc i32 @foo(")
                 != std::string::npos);
}

TETHER_TEST(extern_win64_emits_x86_64_win64cc) {
    auto c = compile(
        "extern \"win64\" fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare x86_64_win64cc i32 @foo(")
                 != std::string::npos);
}

TETHER_TEST(bare_extern_defaults_to_c_convention) {
    // `extern fn foo(...)` without an explicit "C" still defaults to
    // the C calling convention — that matches the design doc's rule
    // that extern "C" is the universal FFI boundary.
    auto c = compile(
        "extern fn foo(x: i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare i32 @foo(") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("ccc ") == std::string::npos);
}

// ---- Real integer widths --------------------------------------------
//
// The v0.2–v0.8 stub widened every integer to i64 "for uniformity".
// v0.9 uses the declared widths: i32 stays i32, i8 stays i8, etc.
// This is required for ABI compatibility with C — a Tether function
// `fn add(a: i32, b: i32) -> i32` must declare itself as
// `define i32 @add(i32, i32)`, not `define i64 @add(i64, i64)`,
// or the call boundary with C is broken.

TETHER_TEST(function_signature_uses_real_widths) {
    auto c = compile(
        "fn add(a: i32, b: i32) -> i32 { return a + b }\n");
    TETHER_CHECK(c.llvm_ir.find("define i32 @_tether_add(i32 %arg0, i32 %arg1)")
                 != std::string::npos);
}

TETHER_TEST(function_signature_u8_u16_u32) {
    auto c = compile(
        "fn f(a: u8, b: u16, c: u32) -> u64 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("define i64 @_tether_f(i8 %arg0, i16 %arg1, i32 %arg2)")
                 != std::string::npos);
}

TETHER_TEST(function_signature_f32_f64) {
    auto c = compile(
        "fn f(a: f32, b: f64) -> f64 { return b }\n");
    TETHER_CHECK(c.llvm_ir.find("define double @_tether_f(float %arg0, double %arg1)")
                 != std::string::npos);
}

TETHER_TEST(return_i8_truncates_i64_literal) {
    // `return 42` — the literal 42 is i64 by default, but the
    // function returns i8. The emitter must insert a trunc.
    auto c = compile(
        "fn f() -> i8 { return 42 }\n");
    TETHER_CHECK(c.llvm_ir.find("define i8 @_tether_f()") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("trunc i64") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("ret i8") != std::string::npos);
}

TETHER_TEST(call_arg_truncates_to_param_type) {
    // Calling a function that takes i8 with an i64 literal must
    // truncate the argument at the call site.
    auto c = compile(
        "fn byte(x: i8) -> i8 { return x }\n"
        "fn f() -> i8 { return byte(255) }\n");
    TETHER_CHECK(c.llvm_ir.find("call i8 @_tether_byte(i8") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("trunc i64") != std::string::npos);
}

// ---- Variadic extern -------------------------------------------------
//
// `extern fn printf(fmt: *const u8, ...) -> i32` must lower to
// `declare i32 @printf(i8*, ...)`. The `...` is part of the function
// type; calls to variadic functions pass extra args after the fixed
// ones.

TETHER_TEST(variadic_extern_decl_has_ellipsis) {
    auto c = compile(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("declare i32 @printf(i8*, ...)") != std::string::npos);
}

TETHER_TEST(variadic_extern_call_passes_extra_args) {
    auto c = compile(
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"
        "fn f() -> i32 {\n"
        "  printf(\"%d %d\", 1, 2)\n"
        "  return 0\n"
        "}\n");
    // The call should pass the format string + two integer args.
    // The extra args go after the fixed fmt parameter.
    TETHER_CHECK(c.llvm_ir.find("call i32 @printf(") != std::string::npos);
    // Verify there are at least 3 args (fmt + 1 + 2). We count
    // commas in the call — should be at least 2.
    size_t call_pos = c.llvm_ir.find("call i32 @printf(");
    TETHER_CHECK(call_pos != std::string::npos);
    size_t close = c.llvm_ir.find(")", call_pos);
    std::string call_args = c.llvm_ir.substr(call_pos, close - call_pos);
    size_t comma_count = 0;
    for (char ch : call_args) if (ch == ',') ++comma_count;
    TETHER_CHECK(comma_count >= 2);
}

// ---- Struct ABI ------------------------------------------------------
//
// A Tether `struct Vec3 { x: f32, y: f32, z: f32 }` must lower to
// `{ float, float, float }` in LLVM IR — not `{ i64, i64, i64 }`.
// This is what makes struct passing through the C ABI zero-overhead:
// the bytes are the same on both sides.

TETHER_TEST(struct_layout_uses_real_field_types) {
    auto c = compile(
        "struct Vec3 { x: f32, y: f32, z: f32 }\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("%struct.Vec3 = type { float, float, float }")
                 != std::string::npos);
}

TETHER_TEST(struct_layout_mixed_types) {
    auto c = compile(
        "struct Mixed { a: i32, b: f64, c: i8 }\n"
        "fn f() -> i32 { return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("%struct.Mixed = type { i32, double, i8 }")
                 != std::string::npos);
}

TETHER_TEST(struct_constructor_stores_real_types) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn make(a: i32, b: i32) -> Point { return Point(a, b) }\n");
    TETHER_CHECK(c.llvm_ir.find("alloca %struct.Point") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("store i32") != std::string::npos);
    // Should NOT have widened i64 stores.
    TETHER_CHECK(c.llvm_ir.find("store i64") == std::string::npos);
}

TETHER_TEST(struct_field_access_loads_real_type) {
    auto c = compile(
        "struct Point { x: i32, y: i32 }\n"
        "fn get_x(p: Point) -> i32 { return p.x }\n");
    TETHER_CHECK(c.llvm_ir.find("load i32, i32*") != std::string::npos);
}

// ---- ref vs *const lowering -----------------------------------------
//
// ref T and *const T both lower to LLVM pointers, but refs carry
// noalias/nonnull/readonly attributes (the borrow checker's
// guarantees encoded as LLVM attributes), while raw pointers carry
// none (they're unsafe, may alias, may be null).

TETHER_TEST(ref_param_carries_noalias_nonnull_readonly) {
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
    TETHER_CHECK(c.llvm_ir.find("readonly") == std::string::npos);
}

TETHER_TEST(raw_ptr_carries_no_attributes) {
    auto c = compile(
        "extern fn foo(p: *const i32) -> i32\n"
        "fn f() -> i32 { return 0 }\n");
    // The declare for foo should have i32* with no attributes.
    size_t pos = c.llvm_ir.find("@foo");
    TETHER_CHECK(pos != std::string::npos);
    std::string around = c.llvm_ir.substr(pos, 60);
    TETHER_CHECK(around.find("noalias") == std::string::npos);
    TETHER_CHECK(around.find("nonnull") == std::string::npos);
}

// ---- End-to-end FFI fixture -----------------------------------------
//
// The existing tests/fixtures/ffi.tether exercises the full FFI
// model from the design doc. v0.9 should compile it cleanly and
// produce IR that links against C's printf/malloc/free directly.

TETHER_TEST(ffi_fixture_compiles_clean) {
    // A trimmed-down version of tests/fixtures/ffi.tether that
    // doesn't require the stdlib loader.
    auto c = compile(
        "module app::foreign_ffi\n"
        "\n"
        "extern \"C\" fn printf(fmt: *const u8, ...) -> i32\n"
        "extern \"C\" fn malloc(size: u64) -> *mut u8\n"
        "extern \"C\" fn free(ptr: *mut u8)\n"
        "\n"
        "fn main() -> i32 {\n"
        "  printf(\"hello\")\n"
        "  return 0\n"
        "}\n");
    // No errors.
    TETHER_CHECK(!c.diag.has_errors());
    // All three externs declared with bare names.
    TETHER_CHECK(c.llvm_ir.find("declare i32 @printf(i8*, ...)") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("declare i8* @malloc(i64)") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("declare void @free(i8*)") != std::string::npos);
    // main calls printf directly.
    TETHER_CHECK(c.llvm_ir.find("call i32 @printf(") != std::string::npos);
    // No _tether_ mangling on any extern symbol.
    TETHER_CHECK(c.llvm_ir.find("@_tether_printf") == std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("@_tether_malloc") == std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("@_tether_free") == std::string::npos);
}

// ---- Internal calls between Tether functions ------------------------
//
// Calls between Tether functions use the `_tether_` mangled name and
// must use the callee's declared param/return types. This is the
// "your ABI" path from the design doc.

TETHER_TEST(internal_call_uses_real_signature) {
    auto c = compile(
        "fn double_it(x: i16) -> i16 { return x + x }\n"
        "fn f() -> i16 { return double_it(21) }\n");
    TETHER_CHECK(c.llvm_ir.find("define i16 @_tether_double_it(i16 %arg0)")
                 != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("call i16 @_tether_double_it(i16")
                 != std::string::npos);
}

TETHER_TEST(void_function_no_return_value) {
    auto c = compile(
        "fn nope() { return }\n"
        "fn f() -> i32 { nope(); return 0 }\n");
    TETHER_CHECK(c.llvm_ir.find("define void @_tether_nope()") != std::string::npos);
    TETHER_CHECK(c.llvm_ir.find("call void @_tether_nope()") != std::string::npos);
}
