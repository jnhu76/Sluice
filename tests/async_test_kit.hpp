#pragma once

// Minimal test substrate: a registry of named test functions, a CHECK macro
// that records failures with file:line, and a run_all entry point with an
// optional substring filter (argv[1]). Plain C++; no external framework;
// single-threaded and deterministic; exit code 1 if any check failed.

#include <cstdio>
#include <string_view>
#include <vector>

namespace sluice_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& check_failures() {
    static int f = 0;
    return f;
}

#define SLUICE_TEST(name_)                                                     \
    static void sluice_test_##name_();                                         \
    static const bool sluice_test_registered_##name_ = [] {                    \
        ::sluice_test::registry().push_back({#name_, &sluice_test_##name_});   \
        return true;                                                           \
    }();                                                                       \
    static void sluice_test_##name_()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++::sluice_test::check_failures();                                 \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,        \
                         __LINE__, #cond);                                     \
        }                                                                      \
    } while (false)

inline int run_all(int argc, char** argv) {
    const std::string_view filter =
        argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    int ran = 0;
    int failed_tests = 0;
    for (const TestCase& tc : registry()) {
        if (!filter.empty() &&
            std::string_view{tc.name}.find(filter) == std::string_view::npos) {
            continue;
        }
        ++ran;
        check_failures() = 0;
        std::fprintf(stdout, "[ RUN ] %s\n", tc.name);
        std::fflush(stdout);
        tc.fn();
        if (check_failures() == 0) {
            std::fprintf(stdout, "[ OK ] %s\n", tc.name);
        } else {
            ++failed_tests;
            std::fprintf(stdout, "[ FAIL ] %s (%d check(s))\n", tc.name,
                         check_failures());
        }
        std::fflush(stdout);
    }
    if (ran == 0) {
        std::fprintf(stdout, "no tests matched filter\n");
    }
    std::fprintf(stdout, "%d test(s) run, %d failed\n", ran, failed_tests);
    std::fflush(stdout);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace sluice_test
