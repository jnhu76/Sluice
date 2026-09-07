#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& failed_check_count() {
    static int count = 0;
    return count;
}

struct TestRegistrar {
    TestRegistrar(const char* name, void (*fn)()) { test_registry().push_back({name, fn}); }
};

inline void report_check(bool ok, const char* expression, const char* file, int line) {
    if (!ok) {
        ++failed_check_count();
        std::printf("  FAILED %s:%d: %s\n", file, line, expression);
    }
}

class TempFile {
  public:
    explicit TempFile(std::string_view content) {
        char name[] = "/tmp/sluice-test-XXXXXX";
        int fd = ::mkstemp(name);
        if (fd < 0) {
            std::abort();
        }
        path_ = name;
        std::size_t written = 0;
        while (written < content.size()) {
            ssize_t n = ::write(fd, content.data() + written, content.size() - written);
            if (n < 0) {
                ::close(fd);
                ::unlink(name);
                std::abort();
            }
            written += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }

    ~TempFile() { ::unlink(path_.c_str()); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const noexcept { return path_; }

  private:
    std::string path_;
};

} // namespace sluice_test

#define SLUICE_TEST(test_name)                                                                    \
    static void sluice_test_body_##test_name();                                                   \
    static ::sluice_test::TestRegistrar sluice_test_registrar_##test_name{                        \
        #test_name, &sluice_test_body_##test_name};                                               \
    static void sluice_test_body_##test_name()

#define SLUICE_CHECK(expression)                                                                  \
    ::sluice_test::report_check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define SLUICE_TEST_MAIN()                                                                        \
    int main() {                                                                                  \
        for (const auto& test_case : ::sluice_test::test_registry()) {                            \
            std::printf("[ run ] %s\n", test_case.name);                                          \
            test_case.fn();                                                                       \
        }                                                                                         \
        if (::sluice_test::failed_check_count() != 0) {                                           \
            std::printf("%d check(s) failed\n", ::sluice_test::failed_check_count());             \
            return 1;                                                                             \
        }                                                                                         \
        std::printf("all checks passed\n");                                                       \
        return 0;                                                                                 \
    }
