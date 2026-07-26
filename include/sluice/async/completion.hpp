// sluice::async::Completion<T> (sluice-CORE-017, ADR §3/§5).
//
// A single outstanding operation's state, CALLER-OWNED so allocation is
// decoupled from submit (mirrors Zig std.Io Completion). The runtime NEVER
// allocates a Completion (L4); the caller constructs one and passes it by
// reference to submit_*.
//
// Lifecycle (ADR §5 L7–L11) — the rules that prevent use-after-free:
//
//   L7.  A Completion is ADDRESS-STABLE while outstanding. It MUST NOT be moved,
//        destroyed, or reused (re-submitted) until it is ready. (This type is
//        non-copyable and non-movable to make that a compile-time guarantee.)
//   L8.  Submitting into a not-ready Completion returns IoError::invalid_state
//        synchronously from submit_* (does not silently overwrite).
//   L9.   result() before ready is a contract violation: debug-mode assertion;
//         release-mode returns IoError::invalid_state (never returns stale data).
//   L11.  Destroying an AsyncIoContext with outstanding Completions is a contract
//         violation (handled in AsyncIoContext, not here).
//
// State machine:
//   idle ──submit_*──> outstanding ──poll/wait_one──> ready
//    ▲                                                    │
//    └──────────────────reset()──────────────────────────┘
//
// `ready()` true means the op has a terminal result (success/error/canceled)
// available via `result()`. Exactly-once: once ready, the backend never mutates
// it again. reset() returns it to idle so it can be reused for a new op.
//
// E15-P1-04 reap sequence: every successful complete_with() stamps the
// Completion with a monotonic reap sequence number (next_reap_seq(), see
// below). This is the narrowest mechanism that lets Batch::next() surface
// completions in actual backend reap order (ADR §6 O2) WITHOUT a new
// AsyncBackend vtable entry: any backend that calls complete_with (which is
// the only path to ready per A3/O1) publishes the order for free. The field
// is read by Batch; ordinary callers ignore it. reset() zeroes it so the
// Completion is reusable. Synchronization matches the existing `state_`
// field: writes occur under AsyncIoContext::access_mtx_ (E7-C) during
// poll()/wait_one(); Batch reads it after the same lock has been released by
// await_one's wait_one() call.
#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace sluice::async {

namespace detail {
// E15-P1-04: process-wide monotonic reap counter, used by Completion::
// complete_with() to stamp a reap sequence on every reaped Completion. Order
// reflects the actual sequence in which backends call complete_with() under
// AsyncIoContext::access_mtx_ (ADR E7-C); Batch::next() consumes it to surface
// completions in true reap order (ADR §6 O2). Relaxed ordering is sufficient:
// the only writer/readers are serialized through the context's access mutex
// (writes) and the Batch's await_one -> next happens-before chain (reads).
//
// F-02 closeout: moved into detail to signal this is an internal mechanism,
// not part of the public API surface. Completion::complete_with() (the sole
// production consumer) calls it inline.
inline std::uint64_t next_reap_seq() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
}  // namespace detail

template <class T>
class Completion {
    // F-02 closeout: reap_seq is an internal ordering mechanism consumed only
    // by Batch::next(). It is not part of the public caller-facing API.
    friend class Batch;

public:
    using value_type = T;

    Completion() = default;
    ~Completion() = default;

    // Non-copyable AND non-movable (L7): an outstanding Completion's address is
    // the backend's handle to it. Move/copy would invalidate that pointer.
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    // --- query ---
    bool ready() const noexcept { return state_ == State::ready; }
    bool outstanding() const noexcept { return state_ == State::outstanding; }
    bool idle() const noexcept { return state_ == State::idle; }

    // ADR L9: result() before ready is a contract violation. Debug asserts;
    // release returns invalid_state rather than stale/garbage.
    Result<T> result() const {
        if (state_ != State::ready) {
            assert(false && "Completion::result() called before ready (L9)");
            return make_unexpected<T>(IoError{IoError::Code::invalid_state});
        }
        return storage_.as_result();
    }

    // --- backend-only mutators (public so AsyncBackend subclasses can mark
    // ready, but documented as not-for-callers) ---
    // Mark outstanding: called by submit_* just before handing to the backend.
    void mark_outstanding() {
        assert(state_ == State::idle &&
               "submit into a non-idle Completion (L8)");
        state_ = State::outstanding;
        storage_ = Storage{};  // clear any prior result
        reap_seq_ = 0;
    }
    // Mark ready with a value (success path) or an error (failure path).
    // E15-P1-04: stamps a monotonic reap sequence so Batch::next() can order
    // completions by actual backend reap order (ADR §6 O2).
    void complete_with(Result<T> res) {
        assert(state_ == State::outstanding &&
               "complete on a non-outstanding Completion (double-completion?)");
        storage_.set(std::move(res));
        reap_seq_ = detail::next_reap_seq();
        state_ = State::ready;
    }
    // Return to idle so the Completion can be reused for a new op.
    void reset() {
        state_ = State::idle;
        storage_ = Storage{};
        reap_seq_ = 0;
    }

private:
    // E15-P1-04: monotonic reap sequence stamped by complete_with(). 0 means
    // "never reaped" (idle or outstanding); a non-zero value orders ready
    // Completions by their actual reap moment. Batch::next() consumes this
    // via the friend grant above; ordinary callers never need it.
    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, outstanding, ready };
    State state_ = State::idle;
    std::uint64_t reap_seq_ = 0;

    // Storage for the terminal result. Holds either a T or an IoError. The
    // partial specialization on void (below) gives Completion<void> a value-less
    // storage so the same state machine works for sync ops.
    struct Storage;
    Storage storage_;
};

// --- CompletionStorage specializations ---------------------------------------

template <class T>
struct Completion<T>::Storage {
    bool has_value = false;
    bool has_error = false;
    T value{};
    IoError error{IoError::Code::backend_error};
    void set(Result<T> r) {
        if (r.has_value()) { value = std::move(r.value()); has_value = true; has_error = false; }
        else { error = r.error(); has_error = true; has_value = false; }
    }
    Result<T> as_result() const {
        if (has_value) return value;
        return make_unexpected<T>(error);
    }
};

// Completion<void> — same lifecycle state machine, but the terminal result
// carries no value (success is just "no error"). Used by sync ops.
template <>
class Completion<void> {
    friend class Batch;

public:
    using value_type = void;

    Completion() = default;
    ~Completion() = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept { return state_ == State::ready; }
    bool outstanding() const noexcept { return state_ == State::outstanding; }
    bool idle() const noexcept { return state_ == State::idle; }

    Result<void> result() const {
        if (state_ != State::ready) {
            assert(false && "Completion::result() called before ready (L9)");
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (has_error_) return make_unexpected<void>(error_);
        return {};
    }

    void mark_outstanding() {
        assert(state_ == State::idle && "submit into a non-idle Completion (L8)");
        state_ = State::outstanding;
        has_error_ = false;
        reap_seq_ = 0;
    }
    void complete_with(Result<void> res) {
        assert(state_ == State::outstanding &&
               "complete on a non-outstanding Completion (double-completion?)");
        if (!res.has_value()) { error_ = res.error(); has_error_ = true; }
        else { has_error_ = false; }
        reap_seq_ = detail::next_reap_seq();
        state_ = State::ready;
    }
    void reset() {
        state_ = State::idle;
        has_error_ = false;
        reap_seq_ = 0;
    }

private:
    // F-02: see Completion<T>::reap_seq().
    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, outstanding, ready };
    State state_ = State::idle;
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
    std::uint64_t reap_seq_ = 0;
};

}  // namespace sluice::async
