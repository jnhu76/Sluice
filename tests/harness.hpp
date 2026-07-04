// Tiny dependency-free test harness for sluice correctness tests.
// Mirrors the spirit of Zig's std.testing.io: deterministic, no external deps.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_test {

inline int& registry_state() {
    static int dummy = 0;
    return dummy;
}

struct Failure {
    std::string file;
    int line;
    std::string expr;
    std::string msg;
};

inline std::vector<Failure>& failures() {
    static std::vector<Failure> f;
    return f;
}

inline void record_failure(const char* file, int line, std::string_view expr,
                           std::string_view msg = {}) {
    failures().push_back({std::string(file), line, std::string(expr), std::string(msg)});
}

inline int report_and_exit() {
    if (failures().empty()) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("FAILED %zu check(s):\n", failures().size());
    for (const auto& f : failures()) {
        std::printf("  %s:%d: %s\n", f.file.c_str(), f.line, f.expr.c_str());
        if (!f.msg.empty()) {
            std::printf("      %s\n", f.msg.c_str());
        }
    }
    return 1;
}

} // namespace sluice_test

#define SLUICE_TEST_CASE(name)                                                                     \
    static void name();                                                                            \
    namespace {                                                                                    \
    struct name##_registrar {                                                                      \
        name##_registrar() { ::sluice_test::run_case(#name, name); }                               \
    };                                                                                             \
    }                                                                                              \
    static name##_registrar name##_registered_;                                                    \
    static void name()

namespace sluice_test {
using test_fn = void (*)();
struct RegisteredCase {
    const char* name;
    test_fn fn;
};
inline std::vector<RegisteredCase>& registered_cases() {
    static std::vector<RegisteredCase> v;
    return v;
}
inline void run_case(const char* name, test_fn fn) {
    registered_cases().push_back({name, fn});
}
inline int run_all() {
    for (const auto& c : registered_cases()) {
        c.fn();
        if (!failures().empty()) {
            std::printf("FAILED in case: %s\n", c.name);
            break;
        }
    }
    return report_and_exit();
}
} // namespace sluice_test

#define SLUICE_CHECK(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::sluice_test::record_failure(__FILE__, __LINE__, #cond);                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SLUICE_CHECK_MSG(cond, msg)                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::sluice_test::record_failure(__FILE__, __LINE__, #cond, msg);                         \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define SLUICE_FAIL(msg)                                                                           \
    do {                                                                                           \
        ::sluice_test::record_failure(__FILE__, __LINE__, "(fail)", msg);                          \
        return;                                                                                    \
    } while (0)

#define SLUICE_MAIN()                                                                              \
    int main() {                                                                                   \
        return ::sluice_test::run_all();                                                           \
    }
