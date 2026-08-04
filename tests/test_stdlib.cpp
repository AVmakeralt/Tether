// tests/test_stdlib.cpp — verify stdlib modules parse and check
//
// Only tests modules that use syntax supported by the current parser.
// Modules using `as` casts, struct literals, or `*mut` in expression
// position will be enabled as the parser grows.

#include "test_framework.hpp"

#include "borrow/borrow.hpp"
#include "check/check.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "module/loader.hpp"
#include "parser/parser.hpp"
#include "resolve/resolve.hpp"
#include "ssa/builder.hpp"
#include "ssa/mono.hpp"
#include "ssa/optimizer.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"
#include "types/types.hpp"

#include <string>

using namespace tether;

static bool module_parses(const std::string& path) {
    InternTable       intern;
    DiagnosticEmitter diag;
    SourceManager     sm;
    Arena             arena;

    module::Loader loader(intern, diag, sm, arena);
    loader.add_search_root(PROJECT_ROOT);

    auto entry = loader.load_entry(std::string(PROJECT_ROOT) + "/" + path);
    if (!entry) return false;
    return !diag.has_errors();
}

// Modules that use only supported syntax (no `as`, no struct literals,
// no `*mut` in expression position).
TETHER_TEST(stdlib_option_parses) {
    TETHER_CHECK(module_parses("stdlib/core/option.tether"));
}

TETHER_TEST(stdlib_result_parses) {
    TETHER_CHECK(module_parses("stdlib/core/result.tether"));
}

TETHER_TEST(stdlib_math_parses) {
    TETHER_CHECK(module_parses("stdlib/core/math.tether"));
}

TETHER_TEST(stdlib_io_parses) {
    TETHER_CHECK(module_parses("stdlib/core/io.tether"));
}

TETHER_TEST(stdlib_time_parses) {
    TETHER_CHECK(module_parses("stdlib/core/time.tether"));
}

TETHER_TEST(stdlib_stack_parses) {
    TETHER_CHECK(module_parses("stdlib/collections/stack.tether"));
}

TETHER_TEST(stdlib_diagnostics_parses) {
    TETHER_CHECK(module_parses("stdlib/diagnostics/diagnostics.tether"));
}

TETHER_TEST(stdlib_cursor_parses) {
    TETHER_CHECK(module_parses("stdlib/parser/cursor.tether"));
}

TETHER_TEST(stdlib_iter_parses) {
    TETHER_CHECK(module_parses("stdlib/iter/iter.tether"));
}

TETHER_TEST(compiler_ir_module_parses) {
    TETHER_CHECK(module_parses("compiler/ir/module.tether"));
}

TETHER_TEST(compiler_optimizer_parses) {
    TETHER_CHECK(module_parses("compiler/optimizer/passes.tether"));
}
