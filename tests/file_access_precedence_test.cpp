#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/file.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

// Pins the frozen initiation precedence of ADR-0002 §5.5 for collisions where
// two or more admission conditions hold at once:
//   closed -> invalid_state
//   access legality -> invalid_argument
//   zero-length -> success 0
//   offset/size validation -> invalid_argument
// A guard reorder that changes any caller-visible outcome below must fail.

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;
using sluice::blocking::read;
using sluice::blocking::read_at;
using sluice::blocking::resize;
using sluice::blocking::write;
using sluice::blocking::write_at;

constexpr std::uint64_t unrepresentable_offset =
    std::numeric_limits<std::uint64_t>::max();

FileOpen mode_with(FileAccess access) {
    FileOpen mode;
    mode.access = access;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_access_precedence_XXXXXX";
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

// Counts every backend read/write entry. A rejected admission must leave
// these at zero: any backend entry means the initiation boundary let an
// illegal operation reach lowering.
class CountingBackend final : public AsyncBackend {
  public:
    int read_entries = 0;
    int write_entries = 0;

    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override {
        return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::not_supported});
    }
    std::size_t outstanding() const noexcept override { return 0; }
    bool supports_request_identity() const noexcept override { return true; }

  private:
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        (void)op;
        (void)c;
        ++read_entries;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        (void)op;
        (void)c;
        ++write_entries;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        (void)op;
        (void)c;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        (void)op;
        (void)c;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
};

template <class T> bool reports_invalid_state(const Result<T>& r) {
    return !r.has_value() && r.error().code == IoError::Code::invalid_state;
}

template <class T> bool reports_invalid_argument(const Result<T>& r) {
    return !r.has_value() && r.error().code == IoError::Code::invalid_argument;
}

// --- blocking: closed + zero-length -> invalid_state, never success 0 ---

bool blocking_read_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(read(file, std::span<std::byte>{}));
}

bool blocking_write_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(write(file, std::span<const std::byte>{}));
}

bool blocking_read_at_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(read_at(file, 0, std::span<std::byte>{}));
}

bool blocking_write_at_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(write_at(file, 0, std::span<const std::byte>{}));
}

// --- blocking: illegal access + zero-length -> invalid_argument, never 0 ---

bool blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    const bool ok = reports_invalid_argument(read(file, std::span<std::byte>{}));
    return file.close().has_value() && ok;
}

bool blocking_write_on_read_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    const bool ok = reports_invalid_argument(write(file, std::span<const std::byte>{}));
    return file.close().has_value() && ok;
}

bool blocking_read_at_on_write_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    const bool ok = reports_invalid_argument(read_at(file, 0, std::span<std::byte>{}));
    return file.close().has_value() && ok;
}

bool blocking_write_at_on_read_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    const bool ok = reports_invalid_argument(write_at(file, 0, std::span<const std::byte>{}));
    return file.close().has_value() && ok;
}

// --- blocking: closed + illegal access -> invalid_state ---

bool blocking_read_on_closed_write_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    std::vector<std::byte> dst(1);
    return reports_invalid_state(read(file, dst));
}

bool blocking_write_on_closed_read_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    const std::byte src{std::byte{0}};
    return reports_invalid_state(write(file, std::span<const std::byte>(&src, 1)));
}

bool blocking_read_at_on_closed_write_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    std::vector<std::byte> dst(1);
    return reports_invalid_state(read_at(file, 0, dst));
}

bool blocking_write_at_on_closed_read_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    const std::byte src{std::byte{0}};
    return reports_invalid_state(write_at(file, 0, std::span<const std::byte>(&src, 1)));
}

// --- blocking: zero-length outranks offset validation ---

bool blocking_read_at_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    auto r = read_at(file, unrepresentable_offset, std::span<std::byte>{});
    const bool ok = r.has_value() && r.value() == 0;
    return file.close().has_value() && ok;
}

bool blocking_write_at_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    auto r = write_at(file, unrepresentable_offset, std::span<const std::byte>{});
    const bool ok = r.has_value() && r.value() == 0;
    return file.close().has_value() && ok;
}

bool blocking_resize_on_closed_read_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(resize(file, 0));
}

bool blocking_resize_on_closed_file_with_unrepresentable_size_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;
    return reports_invalid_state(resize(file, unrepresentable_offset));
}

// --- await: closed + zero-length -> invalid_state, completion stays idle ---

bool await_read_at_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_read_at(file, ctx, 0, std::span<std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool await_write_at_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_write_at(file, ctx, 0, std::span<const std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

// --- await: illegal access + zero-length -> invalid_argument ---

bool await_read_at_on_write_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_read_at(file, ctx, 0, std::span<std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    const bool ok = !result.has_value() &&
                    result.error().code == IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

bool await_write_at_on_read_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_write_at(file, ctx, 0, std::span<const std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    const bool ok = !result.has_value() &&
                    result.error().code == IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

// --- await: closed + illegal access -> invalid_state ---

bool await_read_at_on_closed_write_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(1);
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(await_read_at(file, ctx, 0, dst, c));
        });

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool await_write_at_on_closed_read_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    const std::byte src{std::byte{0}};
    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(await_write_at(file, ctx, 0, std::span<const std::byte>(&src, 1), c));
        });

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

// --- await: zero-length outranks offset validation ---

bool await_read_at_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r = await_read_at(file, ctx, unrepresentable_offset, std::span<std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

bool await_write_at_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto result = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            auto r =
                await_write_at(file, ctx, unrepresentable_offset, std::span<const std::byte>{}, c);
            if (!c.idle()) {
                slot.publish(
                    sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error}));
                return;
            }
            slot.publish(r);
        });

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

// --- explicit submission: rejected admission never enters the backend ---

bool submit_read_rejects_closed_empty_reference_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_write_rejects_closed_empty_reference_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_read_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0}, c);
    const bool admission_ok = reports_invalid_argument(r) && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

bool submit_write_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0}, c);
    const bool admission_ok = reports_invalid_argument(r) && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

bool submit_read_rejects_closed_illegal_access_as_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_write_rejects_closed_illegal_access_as_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

// Access legality is decided before the backend can apply its own offset
// validation: an illegal-access op with an unrepresentable offset must be
// rejected at the boundary with zero backend entries.
bool submit_read_rejects_illegal_access_before_offset_validation() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    std::byte dst{std::byte{0}};
    Completion<std::size_t> c;
    auto r = ctx.submit_read(ReadOp{file, &dst, 1, unrepresentable_offset}, c);
    const bool admission_ok = reports_invalid_argument(r) && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

bool submit_write_rejects_illegal_access_before_offset_validation() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    const std::byte src{std::byte{0}};
    Completion<std::size_t> c;
    auto r = ctx.submit_write(WriteOp{file, &src, 1, unrepresentable_offset}, c);
    const bool admission_ok = reports_invalid_argument(r) && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

// Identity-bearing submission variants share the same admission boundary.
bool submit_read_request_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_read_request(ReadOp{file, nullptr, 0, 0}, c);
    const bool admission_ok = !r.has_value() &&
                              r.error().code == IoError::Code::invalid_argument && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

bool submit_write_request_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_write_request(WriteOp{file, nullptr, 0, 0}, c);
    const bool admission_ok = !r.has_value() &&
                              r.error().code == IoError::Code::invalid_argument && c.idle();
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 &&
           counts->write_entries == 0;
}

// Closed outranks access legality on the identity-bearing variants too: a
// closed reference whose access would be illegal must surface invalid_state
// with zero backend entries.
bool submit_read_request_rejects_closed_illegal_access_as_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_read_request(ReadOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_write_request_rejects_closed_illegal_access_as_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    auto r = ctx.submit_write_request(WriteOp{file, nullptr, 0, 0}, c);
    if (!reports_invalid_state(r))
        return false;
    if (!c.idle())
        return false;
    return counts->read_entries == 0 && counts->write_entries == 0;
}

// --- explicit submission: zero-length completes 0, offset never validated ---

bool submit_zero_length_read_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx(std::move(backend));

    std::byte scratch{std::byte{0}};
    Completion<std::size_t> c;
    auto sr = ctx.submit_read(ReadOp{file, &scratch, 0, unrepresentable_offset}, c);
    if (!sr.has_value())
        return false;
    while (!c.ready())
        (void)ctx.poll();
    auto rr = c.result();
    const bool ok = rr.has_value() && rr.value() == 0;
    return file.close().has_value() && ok;
}

bool submit_zero_length_write_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx(std::move(backend));

    const std::byte scratch{std::byte{0}};
    Completion<std::size_t> c;
    auto sr = ctx.submit_write(WriteOp{file, &scratch, 0, unrepresentable_offset}, c);
    if (!sr.has_value())
        return false;
    while (!c.ready())
        (void)ctx.poll();
    auto rr = c.result();
    const bool ok = rr.has_value() && rr.value() == 0;
    return file.close().has_value() && ok;
}

// Non-zero requests with an unrepresentable offset are rejected at admission
// with the frozen error category, never deferred to backend execution.
bool submit_read_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx(std::move(backend));

    std::byte dst{std::byte{0}};
    Completion<std::size_t> c;
    auto sr = ctx.submit_read(ReadOp{file, &dst, 1, unrepresentable_offset}, c);
    if (sr.has_value()) {
        while (!c.ready())
            (void)ctx.poll();
        (void)c.result();
        c.reset();
        return false;
    }
    const bool rejected = sr.error().code == IoError::Code::invalid_argument && c.idle();
    return file.close().has_value() && rejected;
}

bool submit_write_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx(std::move(backend));

    const std::byte src{std::byte{0}};
    Completion<std::size_t> c;
    auto sr = ctx.submit_write(WriteOp{file, &src, 1, unrepresentable_offset}, c);
    if (sr.has_value()) {
        while (!c.ready())
            (void)ctx.poll();
        (void)c.result();
        c.reset();
        return false;
    }
    const bool rejected = sr.error().code == IoError::Code::invalid_argument && c.idle();
    return file.close().has_value() && rejected;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"blocking_read_closed_with_empty_buffer_reports_invalid_state",
         blocking_read_closed_with_empty_buffer_reports_invalid_state},
        {"blocking_write_closed_with_empty_buffer_reports_invalid_state",
         blocking_write_closed_with_empty_buffer_reports_invalid_state},
        {"blocking_read_at_closed_with_empty_buffer_reports_invalid_state",
         blocking_read_at_closed_with_empty_buffer_reports_invalid_state},
        {"blocking_write_at_closed_with_empty_buffer_reports_invalid_state",
         blocking_write_at_closed_with_empty_buffer_reports_invalid_state},
        {"blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument",
         blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument},
        {"blocking_write_on_read_only_with_empty_buffer_reports_invalid_argument",
         blocking_write_on_read_only_with_empty_buffer_reports_invalid_argument},
        {"blocking_read_at_on_write_only_with_empty_buffer_reports_invalid_argument",
         blocking_read_at_on_write_only_with_empty_buffer_reports_invalid_argument},
        {"blocking_write_at_on_read_only_with_empty_buffer_reports_invalid_argument",
         blocking_write_at_on_read_only_with_empty_buffer_reports_invalid_argument},
        {"blocking_read_on_closed_write_only_reports_invalid_state",
         blocking_read_on_closed_write_only_reports_invalid_state},
        {"blocking_write_on_closed_read_only_reports_invalid_state",
         blocking_write_on_closed_read_only_reports_invalid_state},
        {"blocking_read_at_on_closed_write_only_reports_invalid_state",
         blocking_read_at_on_closed_write_only_reports_invalid_state},
        {"blocking_write_at_on_closed_read_only_reports_invalid_state",
         blocking_write_at_on_closed_read_only_reports_invalid_state},
        {"blocking_read_at_empty_buffer_ignores_unrepresentable_offset",
         blocking_read_at_empty_buffer_ignores_unrepresentable_offset},
        {"blocking_write_at_empty_buffer_ignores_unrepresentable_offset",
         blocking_write_at_empty_buffer_ignores_unrepresentable_offset},
        {"blocking_resize_on_closed_read_only_reports_invalid_state",
         blocking_resize_on_closed_read_only_reports_invalid_state},
        {"blocking_resize_on_closed_file_with_unrepresentable_size_reports_invalid_state",
         blocking_resize_on_closed_file_with_unrepresentable_size_reports_invalid_state},
        {"await_read_at_closed_with_empty_buffer_reports_invalid_state",
         await_read_at_closed_with_empty_buffer_reports_invalid_state},
        {"await_write_at_closed_with_empty_buffer_reports_invalid_state",
         await_write_at_closed_with_empty_buffer_reports_invalid_state},
        {"await_read_at_on_write_only_with_empty_buffer_reports_invalid_argument",
         await_read_at_on_write_only_with_empty_buffer_reports_invalid_argument},
        {"await_write_at_on_read_only_with_empty_buffer_reports_invalid_argument",
         await_write_at_on_read_only_with_empty_buffer_reports_invalid_argument},
        {"await_read_at_on_closed_write_only_reports_invalid_state",
         await_read_at_on_closed_write_only_reports_invalid_state},
        {"await_write_at_on_closed_read_only_reports_invalid_state",
         await_write_at_on_closed_read_only_reports_invalid_state},
        {"await_read_at_empty_buffer_ignores_unrepresentable_offset",
         await_read_at_empty_buffer_ignores_unrepresentable_offset},
        {"await_write_at_empty_buffer_ignores_unrepresentable_offset",
         await_write_at_empty_buffer_ignores_unrepresentable_offset},
        {"submit_read_rejects_closed_empty_reference_before_backend",
         submit_read_rejects_closed_empty_reference_before_backend},
        {"submit_write_rejects_closed_empty_reference_before_backend",
         submit_write_rejects_closed_empty_reference_before_backend},
        {"submit_read_rejects_illegal_access_with_empty_buffer_before_backend",
         submit_read_rejects_illegal_access_with_empty_buffer_before_backend},
        {"submit_write_rejects_illegal_access_with_empty_buffer_before_backend",
         submit_write_rejects_illegal_access_with_empty_buffer_before_backend},
        {"submit_read_rejects_closed_illegal_access_as_invalid_state",
         submit_read_rejects_closed_illegal_access_as_invalid_state},
        {"submit_write_rejects_closed_illegal_access_as_invalid_state",
         submit_write_rejects_closed_illegal_access_as_invalid_state},
        {"submit_read_rejects_illegal_access_before_offset_validation",
         submit_read_rejects_illegal_access_before_offset_validation},
        {"submit_write_rejects_illegal_access_before_offset_validation",
         submit_write_rejects_illegal_access_before_offset_validation},
        {"submit_read_request_rejects_illegal_access_with_empty_buffer_before_backend",
         submit_read_request_rejects_illegal_access_with_empty_buffer_before_backend},
        {"submit_write_request_rejects_illegal_access_with_empty_buffer_before_backend",
         submit_write_request_rejects_illegal_access_with_empty_buffer_before_backend},
        {"submit_read_request_rejects_closed_illegal_access_as_invalid_state",
         submit_read_request_rejects_closed_illegal_access_as_invalid_state},
        {"submit_write_request_rejects_closed_illegal_access_as_invalid_state",
         submit_write_request_rejects_closed_illegal_access_as_invalid_state},
        {"submit_zero_length_read_completes_zero_despite_unrepresentable_offset",
         submit_zero_length_read_completes_zero_despite_unrepresentable_offset},
        {"submit_zero_length_write_completes_zero_despite_unrepresentable_offset",
         submit_zero_length_write_completes_zero_despite_unrepresentable_offset},
        {"submit_read_with_unrepresentable_offset_rejected_at_admission",
         submit_read_with_unrepresentable_offset_rejected_at_admission},
        {"submit_write_with_unrepresentable_offset_rejected_at_admission",
         submit_write_with_unrepresentable_offset_rejected_at_admission},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu file access precedence tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
