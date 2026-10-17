// Request-path submission-failure and terminal-outcome spellings, re-diffed
// against the shared error oracle on the same native faults the direct path
// sees. The expected categories are written out by hand from the error-domain
// rules; nothing here is derived from the implementation's mapping.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/detail/uring_backend.hpp>
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;

int g_failures = 0;
bool g_enospc_injection_ran = false;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::unique_ptr<AsyncBackend> make_backend() {
#if defined(SLUICE_HAS_LIBURING)
    return std::make_unique<UringAsyncBackend>();
#else
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 2});
#endif
}

bool uring_profile_unavailable() {
#if defined(SLUICE_HAS_LIBURING)
    try {
        return !UringAsyncBackend().available();
    } catch (...) {
        return true;
    }
#else
    return false;
#endif
}

template <class T> void drive_ready(AsyncIoContext& ctx, Request<T>& request) {
    while (!request.ready())
        (void)ctx.poll();
}

// An admission-closed context must spell every submission rejection
// invalid_state with no native detail: capacity exhaustion is the only
// admission rejection that may read would_block, and conflating the two
// collapses the admission-domain boundary.
bool admission_closed_rejection_spells_invalid_state() {
    AsyncIoContext ctx = detail::make_context(make_backend());
    ctx.close_admission();
    check(!ctx.admission_open(), "close_admission closes admission");

    auto opened = File::open("/dev/null",
                             FileOpen{.access = FileAccess::read_write, .contents = sluice::FileInitialContents::preserve});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;
    File& file = opened.value();
    const NativeFileRef ref{file};

    std::vector<std::byte> buffer(4, std::byte{0});
    const IoError::Code expected = IoError::Code::invalid_state;

    auto read = ctx.submit_read(ReadOp{ref, buffer.data(), buffer.size(), 0});
    check(!read.has_value() && read.error().code == expected && read.error().os_errno == 0,
          "read after admission close is invalid_state without native detail");

    auto write = ctx.submit_write(WriteOp{ref, buffer.data(), buffer.size(), 0});
    check(!write.has_value() && write.error().code == expected && write.error().os_errno == 0,
          "write after admission close is invalid_state without native detail");

    auto sync_data = ctx.submit_sync_data(SyncDataOp{ref});
    check(!sync_data.has_value() && sync_data.error().code == expected &&
              sync_data.error().os_errno == 0,
          "sync_data after admission close is invalid_state without native detail");

    auto sync_all = ctx.submit_sync_all(SyncAllOp{ref});
    check(!sync_all.has_value() && sync_all.error().code == expected &&
              sync_all.error().os_errno == 0,
          "sync_all after admission close is invalid_state without native detail");

    auto info = ctx.submit_file_info(FileInfoOp{ref});
    check(!info.has_value() && info.error().code == expected && info.error().os_errno == 0,
          "file_info after admission close is invalid_state without native detail");

    auto sized = ctx.submit_size(SizeOp{ref});
    check(!sized.has_value() && sized.error().code == expected && sized.error().os_errno == 0,
          "size after admission close is invalid_state without native detail");

    check(ctx.outstanding() == 0, "the rejected submissions accepted no work");
    return true;
}

// A filesystem no-space fault is an operation result on an accepted request,
// never a submission rejection: the table category and the native errno must
// survive the request path exactly as the direct path reports them.
bool accepted_write_fault_settles_no_space_with_native_detail() {
    auto opened = File::open("/dev/full", FileOpen{.access = FileAccess::write_only});
    if (!opened.has_value()) {
        std::printf("NOT RUN: /dev/full unavailable on this host\n");
        return true;
    }
    g_enospc_injection_ran = true;
    File file = std::move(opened.value());

    AsyncIoContext ctx = detail::make_context(make_backend());
    const NativeFileRef ref{file};
    std::vector<std::byte> buffer(4, std::byte{'x'});
    auto submitted = ctx.submit_write(WriteOp{ref, buffer.data(), buffer.size(), 0});
    check(submitted.has_value(), "a write to a full device is accepted, not rejected");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted.value());
    drive_ready(ctx, request);
    check(request.ready(), "the faulted write still settles to a terminal");
    auto observed = request.take_result();
    check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
          "the terminal is an error result");
    if (!observed.result.has_value()) {
        const IoError e = observed.result.error();
        check(e.code == IoError::Code::no_space && e.os_errno == ENOSPC,
              "request-path write fault maps ENOSPC to no_space with native detail");
    }

    const auto direct = sluice::blocking::write(file, std::span<const std::byte>(buffer));
    check(!direct.has_value() && direct.error().code == IoError::Code::no_space &&
              direct.error().os_errno == ENOSPC,
          "the direct path reports the identical spelling for the same fault");
    check(ctx.outstanding() == 0, "the consumed faulted request leaves no outstanding work");
    return true;
}

// An errno with no canonical mapping stays backend_error but keeps its native
// detail through the request path, again matching the direct spelling.
bool accepted_read_fault_keeps_unmapped_native_detail() {
    auto opened = File::open(".", FileOpen{});
    check(opened.has_value(), "directory fixture open");
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());

    AsyncIoContext ctx = detail::make_context(make_backend());
    const NativeFileRef ref{file};
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{ref, buffer.data(), buffer.size(), 0});
    check(submitted.has_value(), "a read positioned on a directory is accepted, not rejected");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted.value());
    drive_ready(ctx, request);
    check(request.ready(), "the faulted read still settles to a terminal");
    auto observed = request.take_result();
    check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
          "the terminal is an error result");
    if (!observed.result.has_value()) {
        const IoError e = observed.result.error();
        check(e.code == IoError::Code::backend_error && e.os_errno == EISDIR,
              "request-path read fault keeps EISDIR as backend_error with native detail");
    }

    const auto direct = sluice::blocking::read(file, std::span<std::byte>(buffer));
    check(!direct.has_value() && direct.error().code == IoError::Code::backend_error &&
              direct.error().os_errno == EISDIR,
          "the direct path reports the identical spelling for the same fault");
    return true;
}

// The domain boundary itself: a closed resource is a semantic rejection on
// both execution paths and never becomes a settled request, while the faults
// above can never surface before acceptance.
bool closed_resource_is_a_rejection_on_both_paths() {
    auto opened = File::open("/dev/null",
                             FileOpen{.access = FileAccess::read_write,
                                      .contents = sluice::FileInitialContents::preserve});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());
    (void)file.close();

    AsyncIoContext ctx = detail::make_context(make_backend());
    const NativeFileRef ref{file};
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_write(WriteOp{ref, buffer.data(), buffer.size(), 0});
    check(!submitted.has_value() && submitted.error().code == IoError::Code::invalid_state,
          "request path rejects a closed resource before acceptance");

    const auto direct = sluice::blocking::write(file, std::span<const std::byte>(buffer));
    check(!direct.has_value() && direct.error().code == IoError::Code::invalid_state,
          "direct path rejects the same closed resource identically");
    check(ctx.outstanding() == 0, "no accepted work exists on either path");
    return true;
}

}

int main() {
    if (uring_profile_unavailable()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked)\n");
        return 0;
    }
    admission_closed_rejection_spells_invalid_state();
    accepted_write_fault_settles_no_space_with_native_detail();
    accepted_read_fault_keeps_unmapped_native_detail();
    closed_resource_is_a_rejection_on_both_paths();
    if (g_failures != 0) {
        std::fprintf(stderr, "request error domain re-diff: %d failure(s)\n", g_failures);
        return 1;
    }
    if (g_enospc_injection_ran) {
        std::printf("request error domain re-diff passed (fault injections: ENOSPC, EISDIR)\n");
    } else {
        std::printf("request error domain re-diff passed; NOT RUN: ENOSPC fault injection "
                    "(/dev/full unavailable); EISDIR injection ran\n");
    }
    return 0;
}
