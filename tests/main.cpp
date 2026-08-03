// tests/main.cpp — minimal test runner

#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tether::test {

namespace {

std::vector<Failure>& failures_storage() {
    // Construct on first use — immune to static initialization order.
    static std::vector<Failure> failures;
    return failures;
}

int& tests_run_storage() {
    static int n = 0;
    return n;
}

} // namespace

void register_failure(const char* file, int line, const std::string& msg) {
    failures_storage().push_back({file, line, msg});
}

void count_test_run() { ++tests_run_storage(); }

const std::vector<Failure>& failures() {
    return failures_storage();
}

int tests_run() { return tests_run_storage(); }

} // namespace tether::test

int main() {
    using namespace tether::test;

    const auto& fails = failures();
    if (fails.empty()) {
        std::printf("ok — %d test(s) passed\n", tests_run());
        return 0;
    }

    std::printf("FAILED — %zu failure(s) out of %d test(s)\n",
                fails.size(), tests_run());
    for (const auto& f : fails) {
        std::printf("  %s:%d: %s\n", f.file, f.line, f.message.c_str());
    }
    return 1;
}
