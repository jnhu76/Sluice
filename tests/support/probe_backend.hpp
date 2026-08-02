// Minimal test backend for directly driving Completion<T> lifecycle transitions.
//
// ADR-explicit-io-completion-authority: publication mutators (claim/publish)
// are private to Completion<T> and accessible only through AsyncBackend's
// protected helpers. Tests that need to exercise Completion state transitions
// directly (without a full submit/poll cycle) use this backend as the
// authorized driver.
//
// This backend implements NO real I/O. All AsyncBackend virtual methods are
// stubs. Its sole purpose is to expose try_claim/publish through public
// wrappers (claim/publish) for test code.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>

namespace sluice::async {

class ProbeBackend : public AsyncBackend {
public:
    // Drive a Completion's lifecycle from test code:
    //   ProbeBackend pb;
    //   Completion<std::size_t> c;
    //   pb.claim(c);                           // idle → outstanding
    //   pb.publish(c, Result<std::size_t>{42}); // outstanding → ready

    template <class T>
    bool claim(Completion<T>& c) noexcept {
        return try_claim(c);
    }

    template <class T>
    void publish_completion(Completion<T>& c, Result<T>&& r) noexcept {
        publish(c, std::move(r));
    }

    // --- AsyncBackend interface (all stubs — no real I/O) ---
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    std::size_t outstanding() const noexcept override { return 0; }
};

}  // namespace sluice::async
