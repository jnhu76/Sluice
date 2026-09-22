#pragma once

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

// The direct path cannot observe no-opness; deriving it from the request shape
// would fabricate the measurement.
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

// `resize` is driven only for rejected scenarios: an accepted one would mutate
// the fixture every scenario shares.
inline Observation direct_attempt(const AccessFixtures& fixtures, const Input& input) {
    std::vector<std::byte> scratch(input.length, std::byte{0});
    const std::span<std::byte> dst(scratch.data(), input.length);
    const std::span<const std::byte> src(scratch.data(), input.length);

    if (input.closed) {
        // A fresh handle is closed rather than a fixture, which later
        // scenarios still need.
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

inline bool request_drivable(const Input& input) {
    return input.operation == FileOperation::read || input.operation == FileOperation::write ||
           input.operation == FileOperation::sync_data ||
           input.operation == FileOperation::sync_all;
}

// `ready()` captured before any poll() is the no-op measurement: published at
// acceptance versus dispatched, which is the zero-length divergence signal.
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

}
