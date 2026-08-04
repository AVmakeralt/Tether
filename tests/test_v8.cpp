// tests/test_v8.cpp — struct literals, match block bodies, import resolution

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

// ---- Struct literals ----

TETHER_TEST(struct_literal_basic) {
    TETHER_CHECK(parses_clean(
        "struct Point { x: i32, y: i32 }\n"
        "fn f() -> i32 {\n"
        "  let p = Point { x: 1, y: 2 }\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(struct_literal_with_expr_values) {
    TETHER_CHECK(parses_clean(
        "struct Point { x: i32, y: i32 }\n"
        "fn f(a: i32, b: i32) -> i32 {\n"
        "  let p = Point { x: a + 1, y: b * 2 }\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(struct_literal_empty) {
    TETHER_CHECK(parses_clean(
        "struct Empty { }\n"
        "fn f() -> i32 {\n"
        "  let e = Empty { }\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(struct_literal_shorthand) {
    TETHER_CHECK(parses_clean(
        "struct Point { x: i32, y: i32 }\n"
        "fn f(x: i32, y: i32) -> i32 {\n"
        "  let p = Point { x, y }\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(struct_literal_not_confused_with_block) {
    // if x { ... } should NOT be parsed as a struct literal on x
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  if x > 0 { return 1 }\n"
        "  return 0\n"
        "}\n"));
}

TETHER_TEST(struct_literal_not_confused_with_match) {
    // match x { ... } should NOT be parsed as a struct literal on x
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x { 0 => 1, _ => 0 }\n"
        "}\n"));
}

// ---- Match block bodies ----

TETHER_TEST(match_arm_block_body) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    0 => { let y = 1 return y },\n"
        "    _ => { return 0 },\n"
        "  }\n"
        "}\n"));
}

TETHER_TEST(match_arm_mixed_body) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    0 => 1,\n"
        "    1 => { return 2 },\n"
        "    _ => 0,\n"
        "  }\n"
        "}\n"));
}

// ---- Import resolution ----

TETHER_TEST(import_does_not_error) {
    TETHER_CHECK(parses_clean(
        "import std::core::math\n"
        "fn f() -> i32 { return 0 }\n"));
}

TETHER_TEST(import_last_segment_usable) {
    // The import should register 'math' as a name so math::min
    // doesn't produce an "unknown identifier" error.
    TETHER_CHECK(parses_clean(
        "import std::core::math\n"
        "fn f() -> i32 {\n"
        "  let x = math::min(3, 7)\n"
        "  return x\n"
        "}\n"));
}

// ---- Existing features still work ----

TETHER_TEST(pattern_guard_still_works) {
    TETHER_CHECK(parses_clean(
        "fn f(x: i32) -> i32 {\n"
        "  match x { n if n > 0 => 1, _ => 0 }\n"
        "}\n"));
}

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
