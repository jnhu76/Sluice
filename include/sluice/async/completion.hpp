// sluice::async::Completion<T> — caller-owned operation state.
//
// A single outstanding operation's state, CALLER-OWNED so allocation is
// decoupled from submit (mirrors Zig std.Io Completion). The runtime NEVER
// allocates a Completion (L4); the caller constructs one and passes it by
// reference to submit_*.
//
// Completion<T> is an asynchronous terminal-publication cell. Its stored value
// type T MUST support the operations required by the reap/result path: nothrow
// default construction (the idle Storage is value-initialized), nothrow move
// assignment (the reap path assigns the terminal value into storage), copy
// construction (result() returns the stored result BY VALUE — it does not move
// it out), and nothrow destruction (storage is torn down inside the noexcept
// reset()). These requirements are compile-enforced below; Completion<void>
// carries no value and does not impose them.
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
// Internal transients (never caller-visible lifecycle states): query methods
// report `publishing` as outstanding (op not yet reaped); `resetting` reports
// as neither idle nor ready (prior result still being cleared):
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
#include <sluice/async/detail/request_arena.hpp>
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

    // Compile-enforced value-type contract (header docblock). Each nothrow
    // trait below corresponds to a real operation the noexcept reap/reset path
    // performs on a T; a throwing T would escape the noexcept boundary via
    // std::terminate without being a deliberate Completion-authority fail-fast.
    // Copy construction is required by the (non-noexcept) result() return path.
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Completion<T> requires a nothrow default-constructible "
                  "value type (idle storage is value-initialized)");
    // result() returns the stored result by value: Storage::as_result() copies
    // the value into the returned Result<T> (it never moves it out — the
    // caller receives a copy and the Completion keeps its value until reset).
    // Deliberately NOT a nothrow trait: result() is not noexcept, so a throwing
    // copy propagates to the caller like any ordinary return.
    static_assert(std::is_copy_constructible_v<T>,
                  "Completion<T> requires a copy-constructible value type "
                  "(result() returns the stored result by value)");
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
    //
    // Phase B (ADR Decision 15): destroying a READY Completion is allowed and
    // performs the same allocation-free slot release as reset() before the
    // address becomes invalid. The slot was bound at commit (the binding payload
    // holds the arena + slot handle); release returns it to the arena with
    // generation++ under the leaf slot-lifecycle domain. The context/backend
    // must outlive every bound slot (enforced by the arena's
    // request_arena_destruction_fail_fast on non-quiescent destruction).
    ~Completion() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::binding) {
            // Phase B (ADR Decision 5 / I15): destroying a Completion while it is
            // in the private `binding` transient observes a half-installed
            // RequestKey/context/release-capability payload. Only the backend
            // that won the idle -> binding CAS may finish the binding; a
            // destructor cannot. Fail-fast in BOTH Debug and Release.
            detail::completion_binding_destruction_fail_fast();
        }
        if (s == State::outstanding || s == State::publishing ||
            s == State::resetting) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::ready && release_arena_ != nullptr) {
            // Ready-Completion destruction releases the bound slot (Decision 15
            // / design §9 completion_ready -> free authority). The completed-
            // binding release authority fails fast on ANY failure (review I1):
            // a release that silently failed would let this address die while
            // its old slot stays permanently slot_in_use (a later context
            // destruction fail-fast) — an internal protocol violation, not a
            // recoverable user error.
            release_arena_->release_completed_binding(bound_slot_);
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
    // publishing is an internal publication transient: the op is not yet ready,
    // so it still reports as outstanding. resetting is a caller-lifecycle
    // transient: the prior result is being cleared, so it reports as neither
    // idle nor ready (not outstanding) — the Completion is not yet reusable.
    // `binding` (Phase B) is a PRIVATE backend publication window between idle
    // and outstanding: it is neither idle, ready, nor outstanding, so cancel /
    // await / waiter-registration paths that gate on outstanding() observe it
    // as not-yet-accepted and reject synchronously (I15).
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
        if (s == State::binding) {
            // Phase B (ADR Decision 5 / I15): reset() during the private binding
            // transient would observe/tear down a half-installed payload.
            detail::completion_binding_reset_fail_fast();
        }
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
        // Phase B (ADR Decision 15 / design §9): reset() is the slot-release
        // half of the completion_ready -> free transition. The bound slot is
        // returned to the arena (generation++) under the leaf slot-lifecycle
        // domain — allocation-free, no I/O/Scheduler/backend-progress wait, no
        // upward lock. The completed-binding release authority fails fast on
        // ANY failure (review I1): a release that silently failed would let
        // this Completion become reusable while its old slot stays permanently
        // slot_in_use (a later context destruction fail-fast) — an internal
        // protocol violation, not a recoverable user error. Probe-driven
        // Completions (no arena binding) skip this.
        if (release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
        }
        clear_binding_for_backend();
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

    // --- Phase B binding protocol (ADR-explicit-io-request-contract, Accepted,
    //     Decision 5 / I2 / I15). The accepted request lifecycle splits the
    //     single idle -> outstanding CAS into a private two-stage claim so the
    //     winning backend can install RequestKey / ContextIdentity / slot-release
    //     capability before the Completion becomes observable as outstanding.
    //
    //   1. begin_binding_for_backend(): idle -> binding CAS. Exactly one
    //      submitting context wins (the winner of this CAS). Losers return
    //      false and roll back ONLY their own candidate slot; they cannot read
    //      or write the winner's binding payload. While in `binding` the
    //      Completion is NOT observable as outstanding (outstanding()==false),
    //      so cancel / await / waiter-registration reject synchronously.
    //   2. (winner only) install private binding fields (RequestKey, etc.).
    //   3. commit_binding_to_outstanding(): binding -> outstanding RELEASE-STORE.
    //      This is the SUBMIT-SUCCESS LINEARIZATION POINT. An acquire observer
    //      of `outstanding` sees the fully-installed binding.
    //
    // A winner that fails between begin and commit calls
    // rollback_binding_before_accept() (binding -> idle), restoring the
    // Completion to fully reusable idle state with no published binding.
    bool begin_binding_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(
            expected, State::binding,
            std::memory_order::acq_rel,
            std::memory_order::acquire);
    }
    void commit_binding_to_outstanding() noexcept {
        // The winner's binding payload writes happen-before this release-store
        // (program order + release). An acquire-load observer of `outstanding`
        // therefore sees the full binding (I2).
        state_.store(State::outstanding, std::memory_order::release);
    }
    void rollback_binding_before_accept() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(
                expected, State::idle,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            // Not in binding — misuse of the binding rollback authority.
            detail::completion_authority_fail_fast();
        }
    }

    // --- Phase B binding payload (ADR Decision 7 / design §8) ---
    // The idle -> binding CAS winner installs the opaque release capability:
    // the backend-owned arena plus the slot handle (slot index + generation).
    // Ordinary callers cannot forge or replace it (private; negative-compile
    // gate), and reset()/ready-destruction use it to return the slot with
    // generation++ — the completion_ready -> free handshake of design §9.
    // The arena pointer is stable because AsyncIoContext owns its backend via
    // unique_ptr (never moved); the arena destructor fail-fasts if any slot is
    // still bound, so this capability cannot dangle.
    void install_binding_for_backend(detail::RequestArena* arena,
                                     detail::SlotHandle h) noexcept {
        release_arena_ = arena;
        bound_slot_ = h;
    }
    void clear_binding_for_backend() noexcept {
        release_arena_ = nullptr;
        bound_slot_ = {};
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
        idle, binding, outstanding, publishing, ready, resetting
    };
    std::atomic<State> state_{State::idle};
    std::uint64_t reap_seq_ = 0;

    // Phase B binding payload (ADR Decision 7 / I2). Written only by the
    // idle -> binding CAS winner via install_binding_for_backend; observed by
    // reset()/ready destruction. Publish ordering: the payload writes happen
    // before the binding -> outstanding release-store, and the Completion-ready
    // release-store happens after the terminal publication — so reset() (which
    // requires acquire-observing ready) sees the complete payload.
    detail::RequestArena* release_arena_ = nullptr;
    detail::SlotHandle bound_slot_{};

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
    // Returns the stored result BY VALUE: the stored `value` is copied into
    // the returned Result<T> (never moved out — the Completion keeps its copy
    // until reset()). T copy-constructibility is enforced by the Completion<T>
    // static_asserts.
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
        if (s == State::binding) {
            // Phase B (ADR Decision 5 / I15): the binding transient is an
            // exclusive publication window; a destructor cannot finish it.
            detail::completion_binding_destruction_fail_fast();
        }
        if (s == State::outstanding || s == State::publishing ||
            s == State::resetting) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::ready && release_arena_ != nullptr) {
            // Ready-Completion destruction releases the bound slot (Decision 15
            // / design §9). The completed-binding release authority fails fast
            // on ANY failure (review I1 — see the Completion<T> template's
            // destructor note).
            release_arena_->release_completed_binding(bound_slot_);
        }
    }

    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = delete;
    Completion& operator=(Completion&&) = delete;

    bool ready() const noexcept { return state_.load(std::memory_order::acquire) == State::ready; }
    // `binding` (Phase B) reports as neither idle, ready, nor outstanding (see
    // the Completion<T> template's outstanding() note).
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
        if (s == State::binding) {
            // Phase B (ADR Decision 5 / I15): reset during binding observes a
            // half-installed payload.
            detail::completion_binding_reset_fail_fast();
        }
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
        // Phase B (ADR Decision 15 / design §9): reset() releases the bound
        // slot (generation++) under the leaf slot-lifecycle domain, failing
        // fast on ANY release failure (review I1 — see the Completion<T>
        // template's reset() note). Probe-driven Completions (no arena binding)
        // skip this.
        if (release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
        }
        clear_binding_for_backend();
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

    // Phase B binding protocol — see the Completion<T> template's note.
    bool begin_binding_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(
            expected, State::binding,
            std::memory_order::acq_rel,
            std::memory_order::acquire);
    }
    void commit_binding_to_outstanding() noexcept {
        state_.store(State::outstanding, std::memory_order::release);
    }
    void rollback_binding_before_accept() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(
                expected, State::idle,
                std::memory_order::acq_rel,
                std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }

    // Phase B binding payload — see the Completion<T> template's note.
    void install_binding_for_backend(detail::RequestArena* arena,
                                     detail::SlotHandle h) noexcept {
        release_arena_ = arena;
        bound_slot_ = h;
    }
    void clear_binding_for_backend() noexcept {
        release_arena_ = nullptr;
        bound_slot_ = {};
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
        idle, binding, outstanding, publishing, ready, resetting
    };
    std::atomic<State> state_{State::idle};
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
    std::uint64_t reap_seq_ = 0;

    // Phase B binding payload — see the Completion<T> template's note.
    detail::RequestArena* release_arena_ = nullptr;
    detail::SlotHandle bound_slot_{};
};

}  // namespace sluice::async
