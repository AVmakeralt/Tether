// tests/test_types.cpp — type system tests

#include "test_framework.hpp"

#include "support/arena.hpp"
#include "support/intern.hpp"
#include "types/types.hpp"

using namespace tether;
using namespace tether::type;

TETHER_TEST(primitive_types_are_interned) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TETHER_CHECK_EQ(tc.i32(), tc.i32());
    TETHER_CHECK_EQ(tc.u64(), tc.u64());
    TETHER_CHECK_EQ(tc.boolean(), tc.boolean());
    TETHER_CHECK_NE(tc.i32(), tc.i64());
    TETHER_CHECK_NE(tc.i32(), tc.u32());
}

TETHER_TEST(ref_types_are_interned) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TypePtr r1 = tc.make_ref(tc.i32(), false);
    TypePtr r2 = tc.make_ref(tc.i32(), false);
    TypePtr r3 = tc.make_ref(tc.i32(), true);
    TETHER_CHECK_EQ(r1, r2);
    TETHER_CHECK_NE(r1, r3);
}

TETHER_TEST(render_types) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TETHER_CHECK_EQ(tc.render(tc.i32()), std::string("i32"));
    TETHER_CHECK_EQ(tc.render(tc.u64()), std::string("u64"));
    TETHER_CHECK_EQ(tc.render(tc.f64()), std::string("f64"));
    TETHER_CHECK_EQ(tc.render(tc.boolean()), std::string("bool"));
    TETHER_CHECK_EQ(tc.render(tc.void_type()), std::string("void"));
    TETHER_CHECK_EQ(tc.render(tc.make_ref(tc.i32(), false)),
                    std::string("ref i32"));
    TETHER_CHECK_EQ(tc.render(tc.make_ref(tc.i32(), true)),
                    std::string("mut ref i32"));
    TETHER_CHECK_EQ(tc.render(tc.make_raw_ptr(tc.u8(), false)),
                    std::string("*const u8"));
    TETHER_CHECK_EQ(tc.render(tc.make_array(tc.i32(), 4)),
                    std::string("[i32; 4]"));
    TETHER_CHECK_EQ(tc.render(tc.make_tuple({tc.i32(), tc.boolean()})),
                    std::string("(i32, bool)"));
}

TETHER_TEST(render_llvm_types) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TETHER_CHECK_EQ(tc.render_llvm(tc.i32()), std::string("i32"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.f32()), std::string("float"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.f64()), std::string("double"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.boolean()), std::string("i1"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.void_type()), std::string("void"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.make_ref(tc.i32(), false)),
                    std::string("i32*"));
    TETHER_CHECK_EQ(tc.render_llvm(tc.make_array(tc.i32(), 4)),
                    std::string("[4 x i32]"));
}

TETHER_TEST(lookup_primitive_by_name) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TETHER_CHECK_EQ(tc.lookup_primitive("i32"), tc.i32());
    TETHER_CHECK_EQ(tc.lookup_primitive("u64"), tc.u64());
    TETHER_CHECK_EQ(tc.lookup_primitive("f32"), tc.f32());
    TETHER_CHECK_EQ(tc.lookup_primitive("bool"), tc.boolean());
    TETHER_CHECK_EQ(tc.lookup_primitive("void"), tc.void_type());
    TETHER_CHECK_EQ(tc.lookup_primitive("()"), tc.void_type());
    TETHER_CHECK_EQ(tc.lookup_primitive("not_a_type"), nullptr);
}

TETHER_TEST(is_assignable) {
    Arena        arena;
    InternTable  intern;
    TypeContext  tc(arena, intern);

    TETHER_CHECK(is_assignable(tc.i32(), tc.i32()));
    TETHER_CHECK(!is_assignable(tc.i32(), tc.i64()));
    TETHER_CHECK(!is_assignable(tc.i32(), tc.u32()));
    TETHER_CHECK(is_assignable(tc.make_ref(tc.i32(), false),
                                tc.make_ref(tc.i32(), false)));
    TETHER_CHECK(!is_assignable(tc.make_ref(tc.i32(), false),
                                 tc.make_ref(tc.i32(), true)));
    TETHER_CHECK(is_assignable(tc.make_error(), tc.i32()));
    TETHER_CHECK(is_assignable(tc.i32(), tc.make_error()));
}
