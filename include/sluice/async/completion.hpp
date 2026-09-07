#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "tax0_ablation_seams.hpp"
#endif

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sluice::async {

class AsyncBackend;

namespace detail {

inline std::uint64_t next_reap_seq() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
} // namespace detail

template <class T> class Completion {
    friend class Batch;

    friend class AsyncBackend;

  public:
    using value_type = T;

    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Completion<T> requires a nothrow default-constructible "
                  "value type (idle storage is value-initialized)");

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

    ~Completion() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::binding) {
            detail::completion_binding_destruction_fail_fast();
        }
        if (s == State::outstanding || s == State::publishing || s == State::resetting) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::ready && release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
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

    Result<T> result() const {
        if (state_.load(std::memory_order::acquire) != State::ready) {
            assert(false && "Completion::result() called before ready (L9)");
            return make_unexpected<T>(IoError{IoError::Code::invalid_state});
        }
        return storage_.as_result();
    }

    void reset() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::idle)
            return;
        if (s == State::binding) {
            detail::completion_binding_reset_fail_fast();
        }
        if (s != State::ready) {
            detail::completion_authority_fail_fast();
        }

        State expected = State::ready;
        if (!state_.compare_exchange_strong(expected, State::resetting, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }

        if (release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
        }
        clear_binding_for_backend();
        storage_ = Storage{};
        reap_seq_ = 0;

        state_.store(State::idle, std::memory_order::release);
    }

  private:
    bool begin_binding_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(expected, State::binding, std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }
    void commit_binding_to_outstanding() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(expected, State::outstanding,
                                            std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }
    void rollback_binding_before_accept() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(expected, State::idle, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }

    void install_binding_for_backend(detail::RequestArena* arena, detail::SlotHandle h) noexcept {
        release_arena_ = arena;
        bound_slot_ = h;
    }
    void clear_binding_for_backend() noexcept {
        release_arena_ = nullptr;
        bound_slot_ = {};
    }

    void publish_from_reap(Result<T>&& res) noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(expected, State::publishing, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
        storage_.set(std::move(res));
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        if (!detail::tax0_f02_skip_reap_seq())
#endif
            reap_seq_ = detail::next_reap_seq();
        state_.store(State::ready, std::memory_order::release);
    }

    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, binding, outstanding, publishing, ready, resetting };
    std::atomic<State> state_{State::idle};
    std::uint64_t reap_seq_ = 0;

    detail::RequestArena* release_arena_ = nullptr;
    detail::SlotHandle bound_slot_{};

    struct Storage;
    Storage storage_;
};

template <class T> struct Completion<T>::Storage {
    bool has_value = false;
    bool has_error = false;
    T value{};
    IoError error{IoError::Code::backend_error};

    void set(Result<T>&& r) noexcept {
        if (r.has_value()) {
            value = std::move(r.value());
            has_value = true;
            has_error = false;
        } else {
            error = r.error();
            has_error = true;
            has_value = false;
        }
    }

    Result<T> as_result() const {
        if (has_value)
            return value;
        return make_unexpected<T>(error);
    }
};

template <> class Completion<void> {
    friend class Batch;
    friend class AsyncBackend;

  public:
    using value_type = void;

    Completion() = default;

    ~Completion() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::binding) {
            detail::completion_binding_destruction_fail_fast();
        }
        if (s == State::outstanding || s == State::publishing || s == State::resetting) {
            detail::completion_authority_fail_fast();
        }
        if (s == State::ready && release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
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
        if (has_error_)
            return make_unexpected<void>(error_);
        return {};
    }

    void reset() noexcept {
        State s = state_.load(std::memory_order::acquire);
        if (s == State::idle)
            return;
        if (s == State::binding) {
            detail::completion_binding_reset_fail_fast();
        }
        if (s != State::ready) {
            detail::completion_authority_fail_fast();
        }
        State expected = State::ready;
        if (!state_.compare_exchange_strong(expected, State::resetting, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }

        if (release_arena_ != nullptr) {
            release_arena_->release_completed_binding(bound_slot_);
        }
        clear_binding_for_backend();
        has_error_ = false;
        reap_seq_ = 0;
        state_.store(State::idle, std::memory_order::release);
    }

  private:
    bool begin_binding_for_backend() noexcept {
        State expected = State::idle;
        return state_.compare_exchange_strong(expected, State::binding, std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }
    void commit_binding_to_outstanding() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(expected, State::outstanding,
                                            std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }
    void rollback_binding_before_accept() noexcept {
        State expected = State::binding;
        if (!state_.compare_exchange_strong(expected, State::idle, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
    }

    void install_binding_for_backend(detail::RequestArena* arena, detail::SlotHandle h) noexcept {
        release_arena_ = arena;
        bound_slot_ = h;
    }
    void clear_binding_for_backend() noexcept {
        release_arena_ = nullptr;
        bound_slot_ = {};
    }

    void publish_from_reap(Result<void>&& res) noexcept {
        State expected = State::outstanding;
        if (!state_.compare_exchange_strong(expected, State::publishing, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            detail::completion_authority_fail_fast();
        }
        if (!res.has_value()) {
            error_ = res.error();
            has_error_ = true;
        } else {
            has_error_ = false;
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        if (!detail::tax0_f02_skip_reap_seq())
#endif
            reap_seq_ = detail::next_reap_seq();
        state_.store(State::ready, std::memory_order::release);
    }

    std::uint64_t reap_seq() const noexcept { return reap_seq_; }

    enum class State : std::uint8_t { idle, binding, outstanding, publishing, ready, resetting };
    std::atomic<State> state_{State::idle};
    bool has_error_ = false;
    IoError error_{IoError::Code::backend_error};
    std::uint64_t reap_seq_ = 0;

    detail::RequestArena* release_arena_ = nullptr;
    detail::SlotHandle bound_slot_{};
};

} // namespace sluice::async
