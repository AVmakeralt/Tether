// tests/test_framework.hpp — minimal test framework

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace tether::test {

struct Failure {
    const char* file;
    int         line;
    std::string message;
};

void register_failure(const char* file, int line, const std::string& msg);
void count_test_run();

// Accessors for the runner.
const std::vector<Failure>& failures();
int tests_run();

} // namespace tether::test

#define TETHER_TEST(name)                                                      \
    static void name();                                                        \
    namespace {                                                                \
    struct Registrar_##name {                                                  \
        Registrar_##name() {                                                   \
            ::tether::test::count_test_run();                                  \
            try {                                                              \
                name();                                                        \
            } catch (const std::exception& e) {                                \
                ::tether::test::register_failure(__FILE__, __LINE__,           \
                    std::string("exception in " #name ": ") + e.what());       \
            } catch (...) {                                                    \
                ::tether::test::register_failure(__FILE__, __LINE__,           \
                    "unknown exception in " #name);                            \
            }                                                                  \
        }                                                                      \
    } registrar_##name;                                                        \
    }                                                                          \
    static void name()

#define TETHER_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::tether::test::register_failure(__FILE__, __LINE__,               \
                "check failed: " #cond);                                       \
        }                                                                      \
    } while (0)

#define TETHER_CHECK_EQ(a, b)                                                  \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                         \
        if (!(_a == _b)) {                                                     \
            ::tether::test::register_failure(__FILE__, __LINE__,               \
                "check failed: " #a " == " #b);                                \
        }                                                                      \
    } while (0)

#define TETHER_CHECK_NE(a, b)                                                  \
    do {                                                                       \
        auto _a = (a);                                                         \
        auto _b = (b);                                                         \
        if (!(_a != _b)) {                                                     \
            ::tether::test::register_failure(__FILE__, __LINE__,               \
                "check failed: " #a " != " #b);                                \
        }                                                                      \
    } while (0)
