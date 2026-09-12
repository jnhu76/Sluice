// Real-path smoke for the io_uring backend on the canonical operation
// representation: File-referencing ops, access-admission semantics at the
// initiation boundary, and real liburing lowering. Registered only under
// --liburing=y; the macro and link arrive through sluice_async's public
// usage requirement.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/file.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "smoke consumer TU must receive SLUICE_HAS_LIBURING through sluice_async's public usage requirement"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::Result;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_uring_smoke_XXXXXX";
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

// Honest NOT-RUN: the real backend must exist and the kernel must accept
// io_uring setup; otherwise every submission result is meaningless.
bool uring_backend_available() {
    UringAsyncBackend backend;
    return backend.available();
}

bool read_op_through_canonical_file() {
    const std::string path = make_temp_file("uring payload");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    std::vector<std::byte> dst(13);
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto sr = ctx.submit_read(ReadOp{file, dst.data(), dst.size(), 0}, c);
            if (!sr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(wr.error()));
                return;
            }
            slot.publish(c.result());
        });

    if (!result.has_value())
        return false;
    if (result.value() != 13)
        return false;
    if (std::memcmp(dst.data(), "uring payload", 13) != 0)
        return false;
    return file.close().has_value();
}

bool write_op_through_canonical_file() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    const std::byte payload[5] = {std::byte{'w'}, std::byte{'r'}, std::byte{'i'},
                                  std::byte{'t'}, std::byte{'e'}};
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto sr = ctx.submit_write(WriteOp{file, payload, sizeof(payload), 0}, c);
            if (!sr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(wr.error()));
                return;
            }
            slot.publish(c.result());
        });

    if (!result.has_value())
        return false;
    if (result.value() != 5)
        return false;
    return file.close().has_value();
}

bool sync_data_op_through_canonical_file() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<void>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto sr = ctx.submit_sync_data(SyncDataOp{file}, c);
            if (!sr.has_value()) {
                slot.publish(sr);
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(wr);
                return;
            }
            slot.publish(c.result());
        });

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

bool sync_all_op_through_canonical_file() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<void>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<void>>& slot) {
            Completion<void> c;
            auto sr = ctx.submit_sync_all(SyncAllOp{file}, c);
            if (!sr.has_value()) {
                slot.publish(sr);
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(wr);
                return;
            }
            slot.publish(c.result());
        });

    if (!result.has_value())
        return false;
    return file.close().has_value();
}

// Zero-length outranks offset validation on this backend's lowering: a
// zero-length request with an unrepresentable offset must complete 0, not
// fail admission.
bool zero_length_read_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            std::byte scratch{std::byte{0}};
            auto sr = ctx.submit_read(
                ReadOp{file, &scratch, 0, std::numeric_limits<std::uint64_t>::max()}, c);
            if (!sr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(wr.error()));
                return;
            }
            slot.publish(c.result());
        });

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

bool zero_length_write_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            const std::byte scratch{std::byte{0}};
            auto sr = ctx.submit_write(
                WriteOp{file, &scratch, 0, std::numeric_limits<std::uint64_t>::max()}, c);
            if (!sr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
                return;
            }
            auto wr = ctx.await_completion(c);
            if (!wr.has_value()) {
                slot.publish(sluice::make_unexpected<std::size_t>(wr.error()));
                return;
            }
            slot.publish(c.result());
        });

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

// Non-zero requests with an unrepresentable offset are rejected at admission
// with the frozen error category on the real lowering, never deferred to
// backend execution.
bool read_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("d");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            std::byte dst{std::byte{0}};
            auto sr = ctx.submit_read(
                ReadOp{file, &dst, 1, std::numeric_limits<std::uint64_t>::max()}, c);
            if (sr.has_value()) {
                (void)ctx.await_completion(c);
                c.reset();
                slot.publish(sluice::make_unexpected<std::size_t>(
                    sluice::IoError{sluice::IoError::Code::backend_error}));
                return;
            }
            if (!c.idle()) {
                slot.publish(sluice::make_unexpected<std::size_t>(
                    sluice::IoError{sluice::IoError::Code::backend_error}));
                return;
            }
            slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
        });

    const bool ok = !result.has_value() &&
                    result.error().code == sluice::IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

bool write_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, writable_mode()).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<UringAsyncBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            const std::byte src{std::byte{0}};
            auto sr = ctx.submit_write(
                WriteOp{file, &src, 1, std::numeric_limits<std::uint64_t>::max()}, c);
            if (sr.has_value()) {
                (void)ctx.await_completion(c);
                c.reset();
                slot.publish(sluice::make_unexpected<std::size_t>(
                    sluice::IoError{sluice::IoError::Code::backend_error}));
                return;
            }
            if (!c.idle()) {
                slot.publish(sluice::make_unexpected<std::size_t>(
                    sluice::IoError{sluice::IoError::Code::backend_error}));
                return;
            }
            slot.publish(sluice::make_unexpected<std::size_t>(sr.error()));
        });

    const bool ok = !result.has_value() &&
                    result.error().code == sluice::IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

} // namespace

int main() {
    if (!uring_backend_available()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked); "
                    "compile+link verified, runtime probe not executed\n");
        return 0;
    }

    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"read_op_through_canonical_file", read_op_through_canonical_file},
        {"write_op_through_canonical_file", write_op_through_canonical_file},
        {"sync_data_op_through_canonical_file", sync_data_op_through_canonical_file},
        {"sync_all_op_through_canonical_file", sync_all_op_through_canonical_file},
        {"zero_length_read_completes_zero_despite_unrepresentable_offset",
         zero_length_read_completes_zero_despite_unrepresentable_offset},
        {"zero_length_write_completes_zero_despite_unrepresentable_offset",
         zero_length_write_completes_zero_despite_unrepresentable_offset},
        {"read_with_unrepresentable_offset_rejected_at_admission",
         read_with_unrepresentable_offset_rejected_at_admission},
        {"write_with_unrepresentable_offset_rejected_at_admission",
         write_with_unrepresentable_offset_rejected_at_admission},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu uring backend smoke tests passed (real liburing path)\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
