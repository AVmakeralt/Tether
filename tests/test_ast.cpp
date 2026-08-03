// tests/test_ast.cpp — AST node tests

#include "test_framework.hpp"

#include "ast/nodes.hpp"
#include "ast/printer.hpp"
#include "ssa/node.hpp"
#include "support/arena.hpp"
#include "support/intern.hpp"

#include <sstream>

using namespace tether;

TETHER_TEST(arena_allocates_nodes) {
    Arena arena;
    int* a = arena.construct<int>(42);
    int* b = arena.construct<int>(99);
    TETHER_CHECK_EQ(*a, 42);
    TETHER_CHECK_EQ(*b, 99);
    TETHER_CHECK(a != b);
}

TETHER_TEST(arena_calls_destructors) {
    Arena arena;
    // Construct a vector that allocates on the heap. When the arena
    // dies, ~vector() must be called to free the heap allocation.
    std::vector<int>* v = arena.construct<std::vector<int>>();
    for (int i = 0; i < 1000; ++i) v->push_back(i);
    TETHER_CHECK_EQ(v->size(), 1000u);
    // No explicit cleanup; arena destructor handles it.
}

TETHER_TEST(intern_table_dedupes) {
    InternTable intern;
    StrId a = intern.intern(std::string_view("foo"));
    StrId b = intern.intern(std::string_view("foo"));
    StrId c = intern.intern(std::string_view("bar"));
    TETHER_CHECK_EQ(a, b);
    TETHER_CHECK_NE(a, c);
    TETHER_CHECK_EQ(intern.get(a), std::string_view("foo"));
    TETHER_CHECK_EQ(intern.get(c), std::string_view("bar"));
}

TETHER_TEST(intern_empty_string_is_id_zero) {
    InternTable intern;
    StrId e = intern.intern(std::string_view(""));
    TETHER_CHECK_EQ(e, 0u);
}

TETHER_TEST(ssa_opcode_names_exist) {
    // Every opcode should have a non-null name.
    using namespace tether::ssa;
    for (int i = 0; i <= static_cast<int>(Opcode::Trunc); ++i) {
        const char* n = opcode_name(static_cast<Opcode>(i));
        TETHER_CHECK(n != nullptr);
        TETHER_CHECK_NE(std::string(n), std::string("?"));
    }
}
