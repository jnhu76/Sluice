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
#include <sluice/async/detail/request_arena.hpp>
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

    // Roll back a claim that was won but not accepted into backend tracking
    // (ADR §10 / P0-02 bridge): outstanding → idle. Test twin of the io_uring
    // SQE-acquisition-failure path.
    template <class T>
    void rollback_claim(Completion<T>& c) noexcept {
        rollback_claim_before_accept(c);
    }

    // Phase B binding protocol test twins (ADR-explicit-io-request-contract,
    // Accepted, Decision 5). Expose begin/commit/rollback-binding so the
    // lifecycle tests can drive the two-stage claim directly without a full
    // submit/poll cycle. The base-class helpers are reached via AsyncBackend::
    // qualification because these wrapper methods share the same names.
    template <class T>
    bool begin_binding(Completion<T>& c) noexcept {
        return AsyncBackend::begin_binding(c);
    }
    template <class T>
    void commit_binding(Completion<T>& c) noexcept {
        AsyncBackend::commit_binding(c);
    }
    template <class T>
    void rollback_binding(Completion<T>& c) noexcept {
        AsyncBackend::rollback_binding_before_accept(c);
    }
    // Phase B (ADR Decision 7): install/clear the slot-release capability so a
    // probe-driven Completion can exercise the reset/ready-destruction slot
    // release handshake against a real arena.
    template <class T>
    void install_binding(Completion<T>& c, detail::RequestArena* arena,
                         detail::SlotHandle h) noexcept {
        AsyncBackend::install_binding(c, arena, h);
    }
    template <class T>
    void clear_binding(Completion<T>& c) noexcept {
        AsyncBackend::clear_binding(c);
    }

    // Phase B (review C2/C3): type-erased terminal publication thunks, written
    // by a trusted backend-author (they reach the protected AsyncBackend::
    // publish helpers). A probe-driven arena test installs one of these into a
    // slot via RequestArena::install_publication_binding; reap then publishes
    // Completion-ready through it inside the leaf domain.
    static void publish_size_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion),
                              terminal_to_size(t));
    }
    static void publish_void_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<void>*>(completion),
                              terminal_to_void(t));
    }

  private:
    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<std::size_t>(t.error);
        return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
    }
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<void>(t.error);
        return {};
    }

  public:

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
