#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/file.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::make_unexpected;
using sluice::Result;

// Compile-level naming contract: the canonical File path converts implicitly,
// while a raw native handle must spell NativeFileRef explicitly. A bare int
// does not construct an operation.
template <class Op, class... Args>
auto brace_init_detects(int) -> decltype(Op{std::declval<Args>()...}, std::true_type{});
template <class Op, class... Args> auto brace_init_detects(long) -> std::false_type;

static_assert(
    !decltype(brace_init_detects<ReadOp, int, std::byte*, std::size_t, std::uint64_t>(0))::value,
    "ReadOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<WriteOp, int, const std::byte*, std::size_t,
                                           std::uint64_t>(0))::value,
              "WriteOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<SyncDataOp, int>(0))::value,
              "SyncDataOp must not be constructible from a bare fd");
static_assert(!decltype(brace_init_detects<SyncAllOp, int>(0))::value,
              "SyncAllOp must not be constructible from a bare fd");
static_assert(std::is_constructible_v<NativeFileRef, int>,
              "NativeFileRef{int} is the explicitly-named interop path");
static_assert(std::is_convertible_v<File&, NativeFileRef>,
              "canonical File converts implicitly to NativeFileRef");
static_assert(std::is_trivially_copyable_v<NativeFileRef>,
              "NativeFileRef is a value copy of the handle, owning nothing");

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_explicit_ref_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

bool file_content_is(const std::string& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::vector<char> buf(expected.size() + 1, '\0');
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(expected.size()))
        return false;
    return std::memcmp(buf.data(), expected.data(), expected.size()) == 0;
}

bool multiple_outstanding_ops_on_one_canonical_file() {
    const std::string path = make_temp_file("hello world!!");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());

    std::vector<std::byte> head(5);
    std::vector<std::byte> tail(5);
    const std::string patch = "XYZ";
    auto result = run_task_to_result<std::size_t>(
        2, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> read_head;
            Completion<std::size_t> read_tail;
            Completion<std::size_t> write_patch;

            // All three operations reference the same canonical File object and
            // are outstanding at the same time.
            auto sr1 = ctx.submit_read(ReadOp{file, head.data(), head.size(), 0}, read_head);
            if (!sr1.has_value()) {
                slot.publish(make_unexpected<std::size_t>(sr1.error()));
                return;
            }
            auto sr2 = ctx.submit_read(ReadOp{file, tail.data(), tail.size(), 6}, read_tail);
            if (!sr2.has_value()) {
                slot.publish(make_unexpected<std::size_t>(sr2.error()));
                return;
            }
            auto sw = ctx.submit_write(
                WriteOp{file, reinterpret_cast<const std::byte*>(patch.data()), patch.size(), 11},
                write_patch);
            if (!sw.has_value()) {
                slot.publish(make_unexpected<std::size_t>(sw.error()));
                return;
            }

            auto w1 = ctx.await_completion(read_head);
            auto w2 = ctx.await_completion(read_tail);
            auto w3 = ctx.await_completion(write_patch);
            if (!w1.has_value() || !w2.has_value() || !w3.has_value()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            auto r1 = read_head.result();
            auto r2 = read_tail.result();
            auto r3 = write_patch.result();
            read_head.reset();
            read_tail.reset();
            write_patch.reset();
            if (!r1.has_value() || !r2.has_value() || !r3.has_value()) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            if (r1.value() != 5 || r2.value() != 5 || r3.value() != 3) {
                slot.publish(make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state}));
                return;
            }
            slot.publish(r1.value() + r2.value() + r3.value());
        });

    const bool content_ok = file_content_is(path, "hello worldXYZ");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 13)
        return false;
    if (std::memcmp(head.data(), "hello", 5) != 0)
        return false;
    if (std::memcmp(tail.data(), "world", 5) != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool explicit_native_ref_over_raw_fd() {
    const std::string path = make_temp_file("interop payload");
    if (path.empty())
        return false;
    const int raw_fd = ::open(path.c_str(), O_RDONLY);
    if (raw_fd < 0)
        return false;

    std::vector<std::byte> dst(7);
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            // The interop resource is referenced by explicitly naming the
            // mechanism type; the raw fd alone does not construct the op.
            slot.publish(await_read_once(ctx, NativeFileRef{raw_fd}, dst, 8, c));
        });

    ::close(raw_fd);
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 7)
        return false;
    return std::memcmp(dst.data(), "payload", 7) == 0;
}

bool cancel_through_file_referenced_op_reaches_terminal() {
    const std::string path = make_temp_file("cancel target");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());

    bool terminal_observed = false;
    bool follow_up_read_ok = false;

    std::vector<std::byte> dst(4);
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto sr = ctx.submit_read(ReadOp{file, dst.data(), dst.size(), 0}, c);
            if (!sr.has_value()) {
                slot.publish(make_unexpected<std::size_t>(sr.error()));
                return;
            }
            // The read may win the race; both outcomes must reach a terminal.
            // cancel_waiter only wakes the waiter; when it wins the race the
            // drain branch below takes over, otherwise the completion has
            // already published the op's own result.
            (void)ctx.cancel_waiter(c);
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                auto dr = await_drain(ctx, c);
                if (!dr.has_value()) {
                    slot.publish(make_unexpected<std::size_t>(dr.error()));
                    return;
                }
                terminal_observed = true;
            } else {
                auto rr = c.result();
                c.reset();
                if (!rr.has_value()) {
                    if (rr.error().code != IoError::Code::canceled) {
                        slot.publish(make_unexpected<std::size_t>(rr.error()));
                        return;
                    }
                    terminal_observed = true;
                } else {
                    terminal_observed = true;
                }
            }

            // The canonical File reference keeps its authority after the
            // canceled operation: a follow-up read still works.
            auto follow = await_read_at(file, ctx, 7, dst, c);
            follow_up_read_ok = follow.has_value() && follow.value() == 4 &&
                                std::memcmp(dst.data(), "targ", 4) == 0;
            slot.publish(std::size_t{0});
        });

    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (!terminal_observed)
        return false;
    if (!follow_up_read_ok)
        return false;
    return file.close().has_value();
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"multiple_outstanding_ops_on_one_canonical_file",
         multiple_outstanding_ops_on_one_canonical_file},
        {"explicit_native_ref_over_raw_fd", explicit_native_ref_over_raw_fd},
        {"cancel_through_file_referenced_op_reaches_terminal",
         cancel_through_file_referenced_op_reaches_terminal},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu explicit file ref tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
