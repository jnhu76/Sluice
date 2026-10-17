#include "canonical_request_test_support.hpp"
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
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

constexpr std::uint64_t unrepresentable_offset = std::numeric_limits<std::uint64_t>::max();

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

class CountingBackend final : public AsyncBackend {
  public:
    int read_entries = 0;
    int write_entries = 0;

    std::size_t poll() override { return 0; }
    std::size_t outstanding() const noexcept override { return 0; }
    bool internal_work_retired() const noexcept override { return true; }

  private:
    std::size_t slot_capacity() const noexcept override { return 0; }
    Result<sluice::async::detail::RequestKey> submit_read(ReadOp op) override {
        (void)op;
        ++read_entries;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    Result<sluice::async::detail::RequestKey> submit_write(WriteOp op) override {
        (void)op;
        ++write_entries;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    Result<sluice::async::detail::RequestKey> submit_sync_data(SyncDataOp op) override {
        (void)op;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    Result<sluice::async::detail::RequestKey> submit_sync_all(SyncAllOp op) override {
        (void)op;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    Result<sluice::async::detail::RequestKey> submit_file_info(FileInfoOp op) override {
        (void)op;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    Result<sluice::async::detail::RequestKey> submit_size(SizeOp op) override {
        (void)op;
        return sluice::make_unexpected<sluice::async::detail::RequestKey>(
            IoError{IoError::Code::not_supported});
    }
    sluice::async::detail::PublicCancel
    cancel_identity(sluice::async::detail::RequestKey key) override {
        (void)key;
        return sluice::async::detail::PublicCancel::not_found;
    }
};

template <class T> bool reports_invalid_state(const Result<T>& r) {
    return !r.has_value() && r.error().code == IoError::Code::invalid_state;
}

template <class T> bool reports_invalid_argument(const Result<T>& r) {
    return !r.has_value() && r.error().code == IoError::Code::invalid_argument;
}

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

bool scoped_read_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, nullptr, 0, 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool scoped_write_closed_with_empty_buffer_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_write(WriteOp{file, nullptr, 0, 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool scoped_read_on_write_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, nullptr, 0, 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    const bool ok = !result.has_value() && result.error().code == IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

bool scoped_write_on_read_only_with_empty_buffer_reports_invalid_argument() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_write(WriteOp{file, nullptr, 0, 0});
        if (r_admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(r_admitted.error());
    }();

    const bool ok = !result.has_value() && result.error().code == IoError::Code::invalid_argument;
    return file.close().has_value() && ok;
}

bool scoped_read_on_closed_write_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(1);
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted = scope.submit_read(ReadOp{file, dst.data(), dst.size(), 0});
        if (admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(admitted.error());
    }();

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool scoped_write_on_closed_read_only_reports_invalid_state() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());
    if (!file.close().has_value())
        return false;

    const std::byte src{std::byte{0}};
    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto admitted =
            scope.submit_write(WriteOp{file, (std::span<const std::byte>(&src, 1)).data(),
                                       (std::span<const std::byte>(&src, 1)).size(), 0});
        if (admitted.has_value() || ctx.outstanding() != 0)
            return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::backend_error});
        return sluice::make_unexpected<std::size_t>(admitted.error());
    }();

    return !result.has_value() && result.error().code == IoError::Code::invalid_state;
}

bool scoped_read_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r_admitted = scope.submit_read(ReadOp{file, nullptr, 0, unrepresentable_offset});
        if (!r_admitted.has_value())
            return sluice::make_unexpected<std::size_t>(r_admitted.error());
        auto r = sluice::test::wait_and_take(scope, r_admitted.value());
        return r;
    }();

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

bool scoped_write_empty_buffer_ignores_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto result = [&]() -> Result<std::size_t> {
        AsyncIoContext ctx = detail::make_context(std::make_unique<ThreadPoolBackend>());
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        auto r = [&]() -> Result<std::size_t> {
            auto admitted = scope.submit_write(WriteOp{file, nullptr, 0, unrepresentable_offset});
            if (!admitted.has_value())
                return sluice::make_unexpected<std::size_t>(admitted.error());
            return sluice::test::wait_and_take(scope, admitted.value());
        }();
        return r;
    }();

    const bool ok = result.has_value() && result.value() == 0;
    return file.close().has_value() && ok;
}

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
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0});
    if (!reports_invalid_state(r))
        return false;
    return ctx.outstanding() == 0 && counts->read_entries == 0 && counts->write_entries == 0;
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
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0});
    if (!reports_invalid_state(r))
        return false;
    return ctx.outstanding() == 0 && counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_read_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0});
    const bool admission_ok = reports_invalid_argument(r) && ctx.outstanding() == 0;
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_write_rejects_illegal_access_with_empty_buffer_before_backend() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0});
    const bool admission_ok = reports_invalid_argument(r) && ctx.outstanding() == 0;
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 && counts->write_entries == 0;
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
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_read(ReadOp{file, nullptr, 0, 0});
    if (!reports_invalid_state(r))
        return false;
    return ctx.outstanding() == 0 && counts->read_entries == 0 && counts->write_entries == 0;
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
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    auto r = ctx.submit_write(WriteOp{file, nullptr, 0, 0});
    if (!reports_invalid_state(r))
        return false;
    return ctx.outstanding() == 0 && counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_read_rejects_illegal_access_before_offset_validation() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::write_only)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::byte dst{std::byte{0}};
    auto r = ctx.submit_read(ReadOp{file, &dst, 1, unrepresentable_offset});
    const bool admission_ok = reports_invalid_argument(r) && ctx.outstanding() == 0;
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_write_rejects_illegal_access_before_offset_validation() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    const std::byte src{std::byte{0}};
    auto r = ctx.submit_write(WriteOp{file, &src, 1, unrepresentable_offset});
    const bool admission_ok = reports_invalid_argument(r) && ctx.outstanding() == 0;
    const bool closed_after = file.close().has_value();
    return admission_ok && closed_after && counts->read_entries == 0 && counts->write_entries == 0;
}

bool submit_zero_length_read_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::byte scratch{std::byte{0}};
    auto sr = ctx.submit_read(ReadOp{file, &scratch, 0, unrepresentable_offset});
    if (!sr.has_value())
        return false;
    Request<std::size_t> request = std::move(sr).value();
    do {
        (void)ctx.poll();
    } while (!request.ready());
    auto rr = request.take_result();
    const bool ok =
        rr.readiness == RequestReadiness::ready && rr.result.has_value() && rr.result.value() == 0;
    return file.close().has_value() && ok;
}

bool submit_zero_length_write_completes_zero_despite_unrepresentable_offset() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    const std::byte scratch{std::byte{0}};
    auto sr = ctx.submit_write(WriteOp{file, &scratch, 0, unrepresentable_offset});
    if (!sr.has_value())
        return false;
    Request<std::size_t> request = std::move(sr).value();
    do {
        (void)ctx.poll();
    } while (!request.ready());
    auto rr = request.take_result();
    const bool ok =
        rr.readiness == RequestReadiness::ready && rr.result.has_value() && rr.result.value() == 0;
    return file.close().has_value() && ok;
}

bool submit_read_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::byte dst{std::byte{0}};
    auto sr = ctx.submit_read(ReadOp{file, &dst, 1, unrepresentable_offset});
    if (sr.has_value()) {
        Request<std::size_t> request = std::move(sr).value();
        while (!request.ready())
            (void)ctx.poll();
        (void)request.take_result();
        return false;
    }
    const bool rejected =
        sr.error().code == IoError::Code::invalid_argument && ctx.outstanding() == 0;
    return file.close().has_value() && rejected;
}

bool submit_write_with_unrepresentable_offset_rejected_at_admission() {
    const std::string path = make_temp_file("x");
    if (path.empty())
        return false;
    File file = std::move(File::open(path, mode_with(FileAccess::read_write)).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    const std::byte src{std::byte{0}};
    auto sr = ctx.submit_write(WriteOp{file, &src, 1, unrepresentable_offset});
    if (sr.has_value()) {
        Request<std::size_t> request = std::move(sr).value();
        while (!request.ready())
            (void)ctx.poll();
        (void)request.take_result();
        return false;
    }
    const bool rejected =
        sr.error().code == IoError::Code::invalid_argument && ctx.outstanding() == 0;
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
        {"scoped_read_closed_with_empty_buffer_reports_invalid_state",
         scoped_read_closed_with_empty_buffer_reports_invalid_state},
        {"scoped_write_closed_with_empty_buffer_reports_invalid_state",
         scoped_write_closed_with_empty_buffer_reports_invalid_state},
        {"scoped_read_on_write_only_with_empty_buffer_reports_invalid_argument",
         scoped_read_on_write_only_with_empty_buffer_reports_invalid_argument},
        {"scoped_write_on_read_only_with_empty_buffer_reports_invalid_argument",
         scoped_write_on_read_only_with_empty_buffer_reports_invalid_argument},
        {"scoped_read_on_closed_write_only_reports_invalid_state",
         scoped_read_on_closed_write_only_reports_invalid_state},
        {"scoped_write_on_closed_read_only_reports_invalid_state",
         scoped_write_on_closed_read_only_reports_invalid_state},
        {"scoped_read_empty_buffer_ignores_unrepresentable_offset",
         scoped_read_empty_buffer_ignores_unrepresentable_offset},
        {"scoped_write_empty_buffer_ignores_unrepresentable_offset",
         scoped_write_empty_buffer_ignores_unrepresentable_offset},
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
    std::printf("all %zu file access precedence tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
