// tests/test_parser.cpp — parser tests

#include "test_framework.hpp"

#include "ast/printer.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <sstream>

using namespace tether;

static ast::ModulePtr parse(const std::string& src,
                            DiagnosticEmitter& diag,
                            InternTable& intern,
                            SourceManager& sm,
                            Arena& arena) {
    uint32_t fid = sm.load_buffer("<test>", src);
    const SourceFile& f = sm.file(fid);
    Lexer lexer(intern, diag, f);
    auto tokens = lexer.tokenize();
    Parser parser(intern, diag, sm, arena, std::move(tokens));
    return parser.parse_module();
}

TETHER_TEST(parse_empty_module) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("", diag, intern, sm, arena);
    TETHER_CHECK(m != nullptr);
    TETHER_CHECK_EQ(m->items.size(), 0u);
    TETHER_CHECK(!diag.has_errors());
}

TETHER_TEST(parse_module_decl) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("module parser::ast", diag, intern, sm, arena);
    TETHER_CHECK_EQ(m->module_path.size(), 2u);
    TETHER_CHECK_EQ(intern.get(m->module_path[0]), std::string_view("parser"));
    TETHER_CHECK_EQ(intern.get(m->module_path[1]), std::string_view("ast"));
    TETHER_CHECK(!diag.has_errors());
}

TETHER_TEST(parse_simple_fn) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("fn add(a: int, b: int) -> int { return a + b }",
                   diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    TETHER_CHECK_EQ(m->items.size(), 1u);
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.kind, ast::ItemKind::Fn);
    TETHER_CHECK_EQ(intern.get(fn.name), std::string_view("add"));
    TETHER_CHECK_EQ(fn.params.size(), 2u);
    TETHER_CHECK_EQ(fn.body != nullptr, true);
}

TETHER_TEST(parse_struct) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("struct Point { x: int, y: int }",
                   diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& s = *m->items[0];
    TETHER_CHECK_EQ(s.kind, ast::ItemKind::Struct);
    TETHER_CHECK_EQ(intern.get(s.name), std::string_view("Point"));
    TETHER_CHECK_EQ(s.fields.size(), 2u);
    TETHER_CHECK_EQ(intern.get(s.fields[0].name), std::string_view("x"));
    TETHER_CHECK_EQ(intern.get(s.fields[1].name), std::string_view("y"));
}

TETHER_TEST(parse_enum_with_variants) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("enum Color { Red, Green, Blue, Rgb(int, int, int) }",
                   diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& e = *m->items[0];
    TETHER_CHECK_EQ(e.kind, ast::ItemKind::Enum);
    TETHER_CHECK_EQ(e.variants.size(), 4u);
    TETHER_CHECK_EQ(e.variants[3].args.size(), 3u);
}

TETHER_TEST(parse_trait_and_impl) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "trait Hash { fn hash(self) -> u64 }"
        "impl Hash for Foo { fn hash(self) -> u64 { return 0 } }",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    TETHER_CHECK_EQ(m->items.size(), 2u);
    TETHER_CHECK_EQ(m->items[0]->kind, ast::ItemKind::Trait);
    TETHER_CHECK_EQ(m->items[1]->kind, ast::ItemKind::Impl);
    TETHER_CHECK(m->items[1]->impl_trait != nullptr);
    TETHER_CHECK(m->items[1]->impl_type  != nullptr);
}

TETHER_TEST(parse_let_statements) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse("fn f() { let x = 1; let mut y: int = 2; let z = x + y }",
                   diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.body->stmts.size(), 3u);
    TETHER_CHECK_EQ(fn.body->stmts[0]->kind, ast::StmtKind::Let);
    TETHER_CHECK_EQ(fn.body->stmts[1]->let_is_mut, true);
}

TETHER_TEST(parse_match_expr) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "fn f(x: int) -> int {"
        "  match x {"
        "    0 => 1,"
        "    1 => 2,"
        "    _ => 3,"
        "  }"
        "}",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    // The match is the block's trailing expression (no semicolon).
    TETHER_CHECK(fn.body->trailing != nullptr);
    TETHER_CHECK_EQ(fn.body->trailing->kind, ast::ExprKind::Match);
    TETHER_CHECK_EQ(fn.body->trailing->arms.size(), 3u);
}

TETHER_TEST(parse_alloc_arena) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "fn f() {"
        "  alloc arena = Arena();"
        "  let node = alloc arena Node(1, 2);"
        "}",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.body->stmts.size(), 2u);
    TETHER_CHECK_EQ(fn.body->stmts[1]->let_value->kind, ast::ExprKind::Alloc);
}

TETHER_TEST(parse_borrow_move) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "fn f() {"
        "  let x = 1;"
        "  let y = borrow x;"
        "  let z = borrow mut x;"
        "  let w = move x;"
        "}",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.body->stmts[1]->let_value->kind, ast::ExprKind::Borrow);
    TETHER_CHECK_EQ(fn.body->stmts[2]->let_value->kind, ast::ExprKind::Borrow);
    TETHER_CHECK_EQ(fn.body->stmts[3]->let_value->kind, ast::ExprKind::Move);
}

TETHER_TEST(parse_extern_ffi) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "ffi \"stdio.h\""
        "extern fn printf(fmt: *const u8, ...) -> int",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    TETHER_CHECK_EQ(m->items.size(), 1u);
    TETHER_CHECK_EQ(m->items[0]->kind, ast::ItemKind::Extern);
    TETHER_CHECK_EQ(intern.get(m->items[0]->ffi_header), std::string_view("stdio.h"));
    TETHER_CHECK(m->items[0]->extern_decl != nullptr);
}

TETHER_TEST(parse_generic_fn) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "fn max<T: Ord>(a: T, b: T) -> T { return a }",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.type_params.size(), 1u);
    TETHER_CHECK_EQ(intern.get(fn.type_params[0].name), std::string_view("T"));
    TETHER_CHECK_EQ(fn.type_params[0].bounds.size(), 1u);
}

TETHER_TEST(parse_unsafe_block) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "fn f() { unsafe { let x = 1 } }",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    const ast::Item& fn = *m->items[0];
    TETHER_CHECK_EQ(fn.body->stmts.size(), 1u);
    TETHER_CHECK_EQ(fn.body->stmts[0]->kind, ast::StmtKind::Unsafe);
}

TETHER_TEST(parse_print_roundtrip) {
    // Parse a non-trivial program and verify the printer produces
    // *something* without crashing. (Struct literals are not yet in
    // the v0.1 grammar — we use plain function calls here.)
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;
    auto m = parse(
        "module test\n"
        "struct Node { val: int, next: ref Node }\n"
        "impl Node {\n"
        "  fn new(v: int) -> Node { return make(v) }\n"
        "  fn get(self) -> int { return self.val }\n"
        "}\n"
        "fn main() { let n = Node::new(42); let v = n.get() }",
        diag, intern, sm, arena);
    TETHER_CHECK(!diag.has_errors());
    std::ostringstream out;
    ast::Printer printer(out, intern);
    printer.print(*m);
    TETHER_CHECK(out.str().size() > 100);
}
