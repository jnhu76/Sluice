// sluice::async::Completion<T> — caller-owned operation state.
//
// A single outstanding operation's state, CALLER-OWNED so allocation is
// decoupled from submit (mirrors Zig std.Io Completion). The runtime NEVER
// allocates a Completion (L4); the caller constructs one and passes it by
// reference to submit_*.
//
// Authority model (ADR-explicit-io-completion-authority):
//
//   Publication mutators (claim/publish) are PRIVATE. Only AsyncBackend
//   (via friend) can access them. Derived backends use the protected
//   AsyncBackend::try_claim / AsyncBackend::publish static helpers.
//   Ordinary application code CANNOT forge publication transitions.
//
// Lifecycle:
//
//   L7.  A Completion is ADDRESS-STABLE while outstanding. It MUST NOT be moved,
//        destroyed, or reused (re-submitted) until it is ready. (This type is
//        non-copyable and non-movable to make that a compile-time guarantee.)
//   L8.  Submitting into a not-idle Completion returns IoError::invalid_state
//        synchronously from submit_* (does not silently overwrite).
//   L9.  result() before ready is a contract violation: debug-mode assertion;
//        release-mode returns IoError::invalid_state (never returns stale data).
//   L11. Destroying an AsyncIoContext with outstanding Completions is a contract
//        violation (handled in AsyncIoContext, not here).
//
// State machine (ADR-explicit-io-completion-authority §5):
//
//   idle ──backend try_claim (CAS)──> outstanding ──reap publish──> ready
//    ▲                                                                    │
//    └────────────────────────caller reset()──────────────────────────────┘
//
// Forbidden transitions (fail-fast in Debug AND Release):
//   idle → ready, ready → outstanding, outstanding → idle,
//   double publish, double claim, reset on outstanding, destroy outstanding.
//
// `ready()` true means the op has a terminal result (success/error/canceled)
// available via `result()`. Exactly-once: once ready, the backend never mutates
// it again. reset() returns it to idle so it can be reused for a new op.
//
// E15-P1-04 reap sequence: every successful publish stamps the Completion with
// a monotonic reap sequence number (next_reap_seq()). This lets Batch::next()
// surface completions in actual backend reap order (ADR §6 O2). The field is
// read by Batch; ordinary callers ignore it. reset() zeroes it.
#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace sluice::async {

// Forward declaration for friend.
class AsyncBackend;

namespace detail {
// E15-P1-04: process-wide monotonic reap counter, used by Completion::
// publish_from_reap() to stamp a reap sequence on every reaped Completion.
// Order reflects the actual sequence in which backends publish under
// AsyncIoContext::access_mtx_ (ADR E7-C); Batch::next() consumes it to surface
// completions in true reap order (ADR §6 O2). Relaxed ordering is sufficient:
// the only writer/readers are serialized through the context's access mutex
// (writes) and the Batch's await_one -> next happens-before chain (reads).
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

    // ADR-explicit-io-completion-authority §2: publication authority.
    // Only AsyncBackend (and its protected static helpers) may claim/publish.
    friend class AsyncBackend;

public:
    using value_type = T;

    Completion() = default;

    // ADR §8: destruction of an outstanding Completion is a contract violation.
    // Fail-fast in BOTH Debug and Release. The destructor does NOT attempt
    // implicit cancel or drain.
    ~Completion() noexcept {
        if (state_.load(std::memory_order::acquire) == State::outstanding) {
            detail::completion_authority_fail_fast();
        }
    }

    // Non-copyable AND non-movable (L7): an outstanding Completion's address is
    // the backend's handle to it. Move/copy would invalidate that pointer.
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    // --- query (caller-accessible) ---
    bool ready() const noexcept { return state_.load(std::memory_order::acquire) == State::ready; }
    bool outstanding() const noexcept { return state_.load(std::memory_order::acquire) == State::outstanding; }
    bool idle() const noexcept { return state_.load(std::memory_order::acquire) == State::idle; }

    // ADR L9: result() before ready is a contract violation. Debug asserts;
    // release returns invalid_state rather than stale/garbage.
    Result<T> result() const {
        if (state_.load(std::memory_order::acquire) != State::ready) {
            assert(false && "Completion::result() called before ready (L9)");
            return make_unexpected<T>(IoError{IoError::Code::invalid_state});
        }
        return storage_.as_result();
    }

    // --- caller lifecycle (state-checked) ---
    // ADR §7: reset is caller-accessible from ready state (normal reuse) or
    // idle state (defensive no-op, e.g. op_helpers one_step first iteration).
    // reset from outstanding is a contract violation → fail-fast.
    void reset() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::outstanding) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::idle) return;  // already idle, nothing to do
        state_.store(State::idle, std::memory_order::release);
        storage_ = Storage{};
        reap_seq_ = 0;
    }

private:
    // --- backend-only publication mutators (ADR §2, §6, §9) ---
    // These are PRIVATE. AsyncBackend accesses them via friend and exposes
    // protected static helpers (try_claim / publish) to derived backends.

    // Claim: atomic CAS idle → outstanding. Returns true if this caller won
    // the claim; false if the Completion was not idle (another backend/context
    // already claimed it, or it is outstanding/ready). ADR §6: exactly one
    // claim succeeds under concurrent submission.
    bool try_claim_for_backend() noexcept {
        State expected = State::idle;
        if (!state_.compare_exchange_strong(
                expected, State::outstanding,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            return false;
        }
        storage_ = Storage{};  // clear any prior result
        reap_seq_ = 0;
        return true;
    }

    // Publish: transition outstanding → ready with a terminal result.
    // ADR §7: exactly once, single winner, release publication.
    // Fail-fast if not outstanding (double-publish or invalid state).
    void publish_from_reap(Result<T> res) noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s != State::outstanding) {
            detail::completion_authority_fail_fast();
        }
        storage_.set(std::move(res));
        reap_seq_ = detail::next_reap_seq();
        state_.store(State::ready, std::memory_order::release);
    }

    // E15-P1-04: monotonic reap sequence stamped by publish_from_reap().
    // 0 means "never reaped" (idle or outstanding); a non-zero value orders
    // ready Completions by their actual reap moment.
    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, outstanding, ready };
    std::atomic<State> state_{State::idle};
    std::uint64_t reap_seq_ = 0;

    // Storage for the terminal result. Holds either a T or an IoError.
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
    friend class AsyncBackend;

public:
    using value_type = void;

    Completion() = default;

    ~Completion() noexcept {
        if (state_.load(std::memory_order::acquire) == State::outstanding) {
            detail::completion_authority_fail_fast();
        }
    }

    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept { return state_.load(std::memory_order::acquire) == State::ready; }
    bool outstanding() const noexcept { return state_.load(std::memory_order::acquire) == State::outstanding; }
    bool idle() const noexcept { return state_.load(std::memory_order::acquire) == State::idle; }

    Result<void> result() const {
        if (state_.load(std::memory_order::acquire) != State::ready) {
            assert(false && "Completion::result() called before ready (L9)");
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (has_error_) return make_unexpected<void>(error_);
        return {};
    }

    void reset() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::outstanding) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::idle) return;  // already idle, nothing to do
        state_.store(State::idle, std::memory_order::release);
        has_error_ = false;
        reap_seq_ = 0;
    }

private:
    bool try_claim_for_backend() noexcept {
        State expected = State::idle;
        if (!state_.compare_exchange_strong(
                expected, State::outstanding,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            return false;
        }
        has_error_ = false;
        reap_seq_ = 0;
        return true;
    }

    void publish_from_reap(Result<void> res) noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s != State::outstanding) {
            detail::completion_authority_fail_fast();
        }
        if (!res.has_value()) { error_ = res.error(); has_error_ = true; }
        else { has_error_ = false; }
        reap_seq_ = detail::next_reap_seq();
        state_.store(State::ready, std::memory_order::release);
    }

    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, outstanding, ready };
    std::atomic<State> state_{State::idle};
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
    std::uint64_t reap_seq_ = 0;
};

}  // namespace sluice::async
