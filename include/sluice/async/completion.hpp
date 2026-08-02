// sluice::async::Completion<T> — caller-owned operation state.
//
// A single outstanding operation's state, CALLER-OWNED so allocation is
// decoupled from submit (mirrors Zig std.Io Completion). The runtime NEVER
// allocates a Completion (L4); the caller constructs one and passes it by
// reference to submit_*.
//
// Completion<T> is an asynchronous terminal-publication cell. Its stored value
// type T MUST support the non-throwing lifecycle operations required by the
// reap/reset path: nothrow default construction (the idle Storage is value-
// initialized), nothrow move construction and move assignment (the reap path
// moves the terminal Result<T> into storage; result() moves it out), and
// nothrow destruction (storage is torn down inside the noexcept reset()). These
// requirements are compile-enforced below; Completion<void> carries no value
// and does not impose them.
//
// Authority model (ADR-explicit-io-completion-authority):
//
//   Publication mutators (claim/publish/rollback) are PRIVATE. Only
//   AsyncBackend (via friend) can access them. Derived backends use the
//   protected AsyncBackend::try_claim / AsyncBackend::publish /
//   AsyncBackend::rollback_claim_before_accept static helpers. Ordinary
//   non-backend application code cannot forge publication transitions; code
//   that deliberately derives AsyncBackend enters a trusted backend-author
//   role and is granted publication capability (ADR §3).
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
//   idle ──backend try_claim (CAS)──> outstanding ──reap publish (CAS)──> ready
//    ▲                                                                    │
//    └───────────────caller reset() (ready → resetting → idle)────────────┘
//
// Internal transients (never caller-visible lifecycle states; query methods
// report them as outstanding):
//
//   outstanding ──publish CAS──> publishing ──build result──> ready
//   ready ──reset CAS──> resetting ──clear result──> idle
//
// `publishing` makes the winner's storage_/reap_seq_ write exclusive: a
// concurrent publisher loses the CAS and fail-fasts instead of racing a
// half-built result. `resetting` is a caller-lifecycle transient that prevents
// a new claim from observing idle before prior-result cleanup (storage_ /
// reap_seq_) is complete: reset CASes ready → resetting, clears the result,
// then release-stores idle. A new try_claim can only observe idle AFTER the
// cleanup has happened, so claim-vs-reset cannot interleave a half-cleaned
// storage with a freshly-accepted operation.
//
// Concurrency boundary: claim-vs-claim and publish-vs-publish are atomically
// arbitrated (CAS single-winner). reset-vs-new-claim is structurally serialized
// through the resetting transient. The caller MUST NOT race result() or object
// destruction with reset/publication — those are caller-lifecycle errors.
//
// Forbidden transitions (fail-fast in Debug AND Release):
//   idle → ready, ready → outstanding, outstanding → idle (caller reset),
//   publishing → reset, double publish, double claim, reset on
//   outstanding/publishing/resetting, destroy outstanding/publishing/resetting.
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
#include <type_traits>

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

    // Compile-enforced value-type contract (header docblock). Each trait below
    // corresponds to a real operation the noexcept reap/reset path performs on
    // a T; a throwing T would escape the noexcept boundary via std::terminate
    // without being a deliberate Completion-authority fail-fast.
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Completion<T> requires a nothrow default-constructible "
                  "value type (idle storage is value-initialized)");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Completion<T> requires a nothrow move-constructible value "
                  "type (reap moves the terminal Result<T> into storage; "
                  "result() moves it out)");
    static_assert(std::is_nothrow_move_assignable_v<T>,
                  "Completion<T> requires a nothrow move-assignable value type "
                  "(publish_from_reap assigns the terminal value into storage)");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "Completion<T> requires a nothrow-destructible value type "
                  "(storage is torn down inside noexcept reset())");

    Completion() = default;

    // ADR §8: destruction of an outstanding/publishing/resetting Completion is
    // a contract violation. Fail-fast in BOTH Debug and Release. The destructor
    // does NOT attempt implicit cancel or drain. (Concurrent object destruction
    // is itself a caller-lifecycle error; the internal state still keeps a
    // consistent fail-fast line.)
    ~Completion() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::outstanding || s == State::publishing ||
            s == State::resetting) {
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
    // publishing and resetting are internal transients: the op is neither idle
    // nor ready, so report it as outstanding (not yet reaped for publishing;
    // not reusable for resetting).
    bool outstanding() const noexcept {
        State s = state_.load(std::memory_order::acquire);
        return s == State::outstanding || s == State::publishing;
    }
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
    // ADR §5 / AC-13 (as amended): reset() is caller-accessible. From ready it
    // CASes ready → resetting, clears storage_/reap_seq_, then release-stores
    // idle (the resetting transient prevents a new claim from observing idle
    // before cleanup completes). From idle it is an idempotent no-op (defensive
    // first-iteration reset in op_helpers one_step/sync_step). reset from
    // outstanding/publishing/resetting is a contract violation → fail-fast. The
    // idle → no-op decision (NOT fail-fast) is registered in AC-13; it is a
    // deliberate divergence from the "ready → idle ONLY" reading.
    void reset() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::idle) return;  // idempotent no-op (defensive)
        if (s != State::ready) {
            // outstanding / publishing / resetting: contract violation.
            detail::completion_authority_fail_fast();
        }
        // ready → resetting: claim the cleanup authority. A concurrent caller
        // (or a state that moved off ready) loses the CAS and fail-fasts.
        State expected = State::ready;
        if (!state_.compare_exchange_strong(
                expected, State::resetting,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
        storage_ = Storage{};
        reap_seq_ = 0;
        // Publish idle AFTER cleanup so a new claim can never observe an idle
        // Completion whose storage/reap_seq are still being cleared.
        state_.store(State::idle, std::memory_order::release);
    }

private:
    // --- backend-only publication mutators (ADR §2, §6, §9, §10) ---
    // These are PRIVATE. AsyncBackend accesses them via friend and exposes
    // protected static helpers (try_claim / publish / rollback_claim_before_accept)
    // to derived backends.

    // Claim: atomic CAS idle → outstanding. Returns true if this caller won
    // the claim; false if the Completion was not idle (another backend/context
    // already claimed it, or it is outstanding/ready/resetting). ADR §6:
    // exactly one claim succeeds under concurrent submission.
    //
    // The claim does NOT touch storage_/reap_seq_: cleanup belongs to the
    // ready → resetting → idle reset authority. By the time idle is observable
    // a previous reset has already cleared the result, so re-clearing here
    // would both duplicate that work and race a backend that has already
    // published outstanding (the storage_ it is about to write into must not be
    // torn down under it).
    bool try_claim_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(
            expected, State::outstanding,
            std::memory_order::acq_rel,
            std::memory_order::acquire);
    }

    // ADR §10 (P0-02 bridge): roll back a claim that was won but NOT accepted
    // into backend tracking — no register/enqueue/dispatch happened and submit
    // has not returned success; the operation is still entirely userspace-owned
    // (e.g. io_uring SQE acquisition failed after a successful claim). Restores
    // outstanding → idle. Contract: call ONLY immediately after this backend's
    // own successful try_claim_for_backend() and before any tracking step.
    // Fails fast if the Completion is not outstanding (misuse of the rollback
    // authority).
    void rollback_claim_before_accept() noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(
                expected, State::idle,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }

    // Publish: single-winner CAS outstanding → publishing, then build the
    // terminal result and store ready with release. Exactly one publisher wins
    // the CAS; a concurrent publisher (or a publish from idle/ready) fails it
    // and fail-fasts — the transient publishing state guarantees the winner's
    // storage_/reap_seq_ writes are exclusive and never raced.
    //
    // Takes Result<T>&& (not by value): avoids a redundant move-construction of
    // the Result into the parameter. The static_assert traits above cover the
    // move-assign/storage writes that follow.
    void publish_from_reap(Result<T>&& res) noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(
                expected, State::publishing,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
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

    enum class State : std::uint8_t {
        idle, outstanding, publishing, ready, resetting
    };
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
    // Move the terminal Result<T> into storage. noexcept is justified by the
    // Completion<T> static_assert traits (T nothrow move-assignable); IoError
    // is trivially copyable.
    void set(Result<T>&& r) noexcept {
        if (r.has_value()) { value = std::move(r.value()); has_value = true; has_error = false; }
        else { error = r.error(); has_error = true; has_value = false; }
    }
    Result<T> as_result() const {
        if (has_value) return value;
        return make_unexpected<T>(error);
    }
};

// Completion<void> — same lifecycle state machine, but the terminal result
// carries no value (success is just "no error"). Used by sync ops. It carries
// no T, so the Completion<T> value-type static_asserts do not apply; the
// bool/IoError publication path stays noexcept by construction.
template <>
class Completion<void> {
    friend class Batch;
    friend class AsyncBackend;

public:
    using value_type = void;

    Completion() = default;

    ~Completion() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::outstanding || s == State::publishing ||
            s == State::resetting) {
            detail::completion_authority_fail_fast();
        }
    }

    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept { return state_.load(std::memory_order::acquire) == State::ready; }
    bool outstanding() const noexcept {
        State s = state_.load(std::memory_order::acquire);
        return s == State::outstanding || s == State::publishing;
    }
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
        if (s == State::idle) return;  // idempotent no-op (defensive)
        if (s != State::ready) {
            detail::completion_authority_fail_fast();
        }
        State expected = State::ready;
        if (!state_.compare_exchange_strong(
                expected, State::resetting,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
        has_error_ = false;
        reap_seq_ = 0;
        state_.store(State::idle, std::memory_order::release);
    }

private:
    bool try_claim_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(
            expected, State::outstanding,
            std::memory_order::acq_rel,
            std::memory_order::acquire);
    }

    void rollback_claim_before_accept() noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(
                expected, State::idle,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }

    void publish_from_reap(Result<void>&& res) noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(
                expected, State::publishing,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
        if (!res.has_value()) { error_ = res.error(); has_error_ = true; }
        else { has_error_ = false; }
        reap_seq_ = detail::next_reap_seq();
        state_.store(State::ready, std::memory_order::release);
    }

    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t {
        idle, outstanding, publishing, ready, resetting
    };
    std::atomic<State> state_{State::idle};
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
    std::uint64_t reap_seq_ = 0;
};

}  // namespace sluice::async
