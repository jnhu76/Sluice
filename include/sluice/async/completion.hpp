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
#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace sluice::async {

template <class T>
class Completion {
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
    }
    // Mark ready with a value (success path) or an error (failure path).
    void complete_with(Result<T> res) {
        assert(state_ == State::outstanding &&
               "complete on a non-outstanding Completion (double-completion?)");
        storage_.set(std::move(res));
        state_ = State::ready;
    }
    // Return to idle so the Completion can be reused for a new op.
    void reset() {
        state_ = State::idle;
        storage_ = Storage{};
    }

private:
    enum class State : std::uint8_t { idle, outstanding, ready };
    State state_ = State::idle;

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
    }
    void complete_with(Result<void> res) {
        assert(state_ == State::outstanding &&
               "complete on a non-outstanding Completion (double-completion?)");
        if (!res.has_value()) { error_ = res.error(); has_error_ = true; }
        else { has_error_ = false; }
        state_ = State::ready;
    }
    void reset() {
        state_ = State::idle;
        has_error_ = false;
    }

private:
    enum class State : std::uint8_t { idle, outstanding, ready };
    State state_ = State::idle;
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
};

}  // namespace sluice::async
