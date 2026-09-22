#include <sluice/file_resource.hpp>

#include "file_test_seams.hpp"

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::file_testing::kCloseCall;
using sluice::file_testing::NativeCall;
using sluice::file_testing::NativeScript;

std::string make_temp_path() {
    char path[] = "/tmp/sluice_close_semantics_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

FileOpen read_write_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

bool descriptor_is_live(int fd) { return ::fcntl(fd, F_GETFD) >= 0; }

bool unarmed_close_releases_the_descriptor() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();
    const bool live_before = descriptor_is_live(fd);

    bool ok = live_before && file.close().has_value();
    ok = ok && !file.is_open();
    ok = ok && !descriptor_is_live(fd);
    ok = ok && file.close().has_value();
    ::unlink(path.c_str());
    return ok;
}

bool close_success_consumes_ownership() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();

    bool ok = true;
    {
        NativeScript script(kCloseCall, fd, {{0, 0}});
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 1;
        ok = ok && script.last_fd() == fd;
        ok = ok && !file.is_open();
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 1;
    }
    ::close(fd); // the scripted close never reached the kernel
    ::unlink(path.c_str());
    return ok;
}

bool close_eintr_consumes_ownership_without_retry() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();

    bool ok = true;
    {
        NativeScript script(kCloseCall, fd, {{-1, EINTR}});
        auto closed = file.close();
        ok = ok && !closed.has_value();
        ok = ok && closed.error().code == IoError::Code::interrupted;
        ok = ok && closed.error().os_errno == EINTR;
        ok = ok && script.calls() == 1;
        ok = ok && !file.is_open();
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 1;
    }
    ::close(fd);
    ::unlink(path.c_str());
    return ok;
}

bool close_error_is_observable_and_consumes_ownership() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();

    bool ok = true;
    {
        NativeScript script(kCloseCall, fd, {{-1, EIO}});
        auto closed = file.close();
        ok = ok && !closed.has_value();
        ok = ok && closed.error().code == IoError::Code::backend_error;
        ok = ok && closed.error().os_errno == EIO;
        ok = ok && script.calls() == 1;
        ok = ok && !file.is_open();
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 1;
    }
    ::close(fd);
    ::unlink(path.c_str());
    return ok;
}

bool destructor_close_failure_is_noexcept_single_attempt() {
    static_assert(std::is_nothrow_destructible_v<File>,
                  "destruction is deterministic cleanup and cannot throw");
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    const int fd = opened.value().native_handle();

    std::size_t attempts = 0;
    {
        NativeScript script(kCloseCall, fd, {{-1, EIO}});
        {
            File doomed = std::move(opened).value();
            (void)doomed;
        }
        attempts = script.calls();
    }
    ::close(fd);
    ::unlink(path.c_str());
    return attempts == 1;
}

bool move_assignment_best_effort_closes_the_old_resource() {
    static_assert(std::is_nothrow_move_assignable_v<File>,
                  "move assignment is a noexcept resource transfer");
    const std::string path_a = make_temp_path();
    const std::string path_b = make_temp_path();
    if (path_a.empty() || path_b.empty())
        return false;
    auto opened_a = File::open(path_a, read_write_mode());
    auto opened_b = File::open(path_b, read_write_mode());
    if (!opened_a.has_value() || !opened_b.has_value())
        return false;
    File dst = std::move(opened_a).value();
    File src = std::move(opened_b).value();
    const int fd_a = dst.native_handle();
    const int fd_b = src.native_handle();

    bool ok = fd_a != fd_b;
    {
        NativeScript script(kCloseCall, fd_a, {{-1, EIO}});
        dst = std::move(src);
        ok = ok && script.calls() == 1;
        ok = ok && script.last_fd() == fd_a;
    }
    ok = ok && dst.native_handle() == fd_b;
    ok = ok && !src.is_open();
    ok = ok && descriptor_is_live(fd_b);
    ok = ok && dst.close().has_value();
    ::close(fd_a); // the scripted close never reached the kernel
    ::unlink(path_a.c_str());
    ::unlink(path_b.c_str());
    return ok;
}

// A retried close could close a descriptor number the kernel has already
// handed to another owner.
bool consumed_close_never_retries_a_reused_descriptor() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path, read_write_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();

    bool ok = true;
    {
        NativeScript script(kCloseCall, fd, {{-1, EINTR}});
        auto closed = file.close();
        ok = ok && !closed.has_value();
        ::close(fd); // stands in for the kernel having released the descriptor
        const int replacement = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        ok = ok && replacement >= 0;
        // Linux hands out the lowest free descriptor; asserting the reuse keeps
        // this case from passing without ever creating it.
        ok = ok && replacement == fd;
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 1;
        ok = ok && descriptor_is_live(replacement);
        if (replacement >= 0)
            ::close(replacement);
    }
    ::unlink(path.c_str());
    return ok;
}

bool close_on_closed_file_is_not_a_native_call() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    const int fd = file.native_handle();
    if (!file.close().has_value())
        return false;

    bool ok = true;
    {
        NativeScript script(kCloseCall, fd, {{-1, EIO}});
        ok = ok && file.close().has_value();
        ok = ok && script.calls() == 0;
    }
    ::unlink(path.c_str());
    return ok;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"unarmed_close_releases_the_descriptor", unarmed_close_releases_the_descriptor},
        {"close_success_consumes_ownership", close_success_consumes_ownership},
        {"close_eintr_consumes_ownership_without_retry",
         close_eintr_consumes_ownership_without_retry},
        {"close_error_is_observable_and_consumes_ownership",
         close_error_is_observable_and_consumes_ownership},
        {"destructor_close_failure_is_noexcept_single_attempt",
         destructor_close_failure_is_noexcept_single_attempt},
        {"move_assignment_best_effort_closes_the_old_resource",
         move_assignment_best_effort_closes_the_old_resource},
        {"consumed_close_never_retries_a_reused_descriptor",
         consumed_close_never_retries_a_reused_descriptor},
        {"close_on_closed_file_is_not_a_native_call", close_on_closed_file_is_not_a_native_call},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu canonical File close-semantics tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
