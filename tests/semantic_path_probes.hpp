#pragma once

// Path adapters for the A1 reference harness.
//
// Each adapter reports only what its execution path observed. None of them
// inspects an expectation, and none of them owns an expected outcome: the
// oracle in `semantic_oracle_harness.hpp` is the only source of requirements.

#include "semantic_oracle_harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace sluice_semantic {

// The direct path reports no no-op observation: nothing outside a direct call
// reveals whether it made a syscall, and inferring it from the request shape
// would assert what the harness is supposed to measure.
inline Observation apply_direct(const sluice::File& file, const Input& input,
                               std::span<std::byte> dst, std::span<const std::byte> src) {
    switch (input.operation) {
    case FileOperation::read:
        return observe_result(sluice::blocking::read_at(file, input.offset, dst));
    case FileOperation::write:
        return observe_result(sluice::blocking::write_at(file, input.offset, src));
    case FileOperation::file_info:
        return observe_result(sluice::blocking::size(file));
    case FileOperation::resize:
        return observe_result(sluice::blocking::resize(file, 0));
    case FileOperation::sync_data:
        return observe_result(sluice::blocking::sync_data(file));
    case FileOperation::sync_all:
        return observe_result(sluice::blocking::sync_all(file));
    }
    return observe_rejection(IoError{.code = sluice::IoError::Code::invalid_argument});
}

// Direct execution: sluice::blocking::* on the canonical File.
//
// `resize` is only driven for rejected scenarios: an accepted resize would
// mutate the shared fixture, and the fixtures exist to serve many scenarios.
inline Observation direct_attempt(const AccessFixtures& fixtures, const Input& input) {
    std::vector<std::byte> scratch(input.length, std::byte{0});
    const std::span<std::byte> dst(scratch.data(), input.length);
    const std::span<const std::byte> src(scratch.data(), input.length);

    if (input.closed) {
        // A closed canonical File: a fresh handle is opened and closed so the
        // operation sees the state a caller would, without disturbing fixtures.
        sluice::FileOpen mode;
        mode.existence = sluice::FileExistence::open_existing;
        mode.access = sluice::FileAccess::read_write;
        auto opened = sluice::File::open(fixtures.path(), mode);
        if (!opened.has_value())
            return observe_rejection(opened.error());
        sluice::File closed = std::move(opened.value());
        (void)closed.close();
        return apply_direct(closed, input, dst, src);
    }

    const sluice::File* file = fixtures.for_access(input.access);
    if (file == nullptr)
        return observe_rejection(IoError{.code = sluice::IoError::Code::invalid_state});
    return apply_direct(*file, input, dst, src);
}

// Only the operations a request backend actually exposes can be driven here.
// resize and file_info have no request form in this commit; SEM-01 does require
// file_info/size on request paths, so that part of the operation matrix stays
// with the ThreadPool/io_uring profile GAP rows. Scenarios using an operation
// no backend exposes yet are reported as not drivable rather than silently
// passing.
inline bool request_drivable(const Input& input) {
    return input.operation == FileOperation::read || input.operation == FileOperation::write ||
           input.operation == FileOperation::sync_data ||
           input.operation == FileOperation::sync_all;
}

// A request execution: submit through a context, then drive it until the
// operation reaches a terminal so no outstanding work is left behind.
//
// The no-op observation is measured, not asserted: a terminal that is already
// `ready()` before any progress call was published at acceptance, which is what
// SEM-03 requires of a logical no-op. A completion that only becomes ready after
// `poll()` was produced by a dispatched operation. The request path therefore
// reports a real observation for every input, and a zero-length request that is
// dispatched shows up as a divergence.
class RequestProbe {
  public:
    explicit RequestProbe(std::unique_ptr<sluice::async::AsyncBackend> backend)
        : ctx_(std::move(backend)) {}

    sluice::async::AsyncIoContext& context() { return ctx_; }

    Observation attempt(const AccessFixtures& fixtures, const Input& input) {
        const sluice::File* file = input.closed ? nullptr : fixtures.for_access(input.access);
        if (!input.closed && file == nullptr)
            return observe_rejection(IoError{.code = sluice::IoError::Code::invalid_state});

        std::vector<std::byte> scratch(input.length, std::byte{0});
        std::byte* buffer = scratch.data();

        sluice::async::NativeFileRef ref =
            input.closed ? sluice::async::NativeFileRef{-1, input.access}
                         : sluice::async::NativeFileRef{*file};

        if (input.operation == FileOperation::read) {
            sluice::async::Completion<std::size_t> c;
            auto submitted = ctx_.submit_read(
                sluice::async::ReadOp{ref, buffer, input.length, input.offset}, c);
            if (!submitted.has_value())
                return observe_rejection(submitted.error());
            const bool published_at_acceptance = c.ready();
            drain_size(c);
            return observe_result(c.result(), published_at_acceptance);
        }
        if (input.operation == FileOperation::write) {
            sluice::async::Completion<std::size_t> c;
            auto submitted = ctx_.submit_write(
                sluice::async::WriteOp{ref, buffer, input.length, input.offset}, c);
            if (!submitted.has_value())
                return observe_rejection(submitted.error());
            const bool published_at_acceptance = c.ready();
            drain_size(c);
            return observe_result(c.result(), published_at_acceptance);
        }
        sluice::async::Completion<void> c;
        auto submitted = input.operation == FileOperation::sync_data
                             ? ctx_.submit_sync_data(sluice::async::SyncDataOp{ref}, c)
                             : ctx_.submit_sync_all(sluice::async::SyncAllOp{ref}, c);
        if (!submitted.has_value())
            return observe_rejection(submitted.error());
        while (!c.ready())
            (void)ctx_.poll();
        return observe_result(c.result());
    }

  private:
    void drain_size(sluice::async::Completion<std::size_t>& c) {
        while (!c.ready())
            (void)ctx_.poll();
    }

    sluice::async::AsyncIoContext ctx_;
};

} // namespace sluice_semantic
