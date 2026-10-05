#pragma once

// Internal, source-local view shared by the UringAsyncBackend translation
// units (uring_backend.cpp, uring_transport.cpp, uring_completion.cpp). It
// must not be installed or included outside src/async.

#include <sluice/async/uring_backend.hpp>
#include <sluice/measurement.hpp>

#if defined(SLUICE_HAS_LIBURING)

#include <liburing.h>

#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <vector>

// The STATX_* mask bits live with the struct statx definition in glibc's
// <sys/stat.h>; provide them only if this environment's headers do not.
#ifndef STATX_TYPE
#define STATX_TYPE 0x00000001u
#endif
#ifndef STATX_MODE
#define STATX_MODE 0x00000002u
#endif
#ifndef STATX_SIZE
#define STATX_SIZE 0x00000200u
#endif
#ifndef STATX_INO
#define STATX_INO 0x00000400u
#endif

namespace sluice::async {

struct UringRingState {
    ::io_uring ring{};
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    UringBackendSubmitTestHooks test_hooks{};
#endif
};

inline constexpr std::uint64_t CONTROL_TAG = std::uint64_t{1} << 63u;
inline constexpr std::uint64_t COOKIE_MASK = CONTROL_TAG - 1u;

inline constexpr bool is_control_cookie(std::uint64_t user_data) noexcept {
    return (user_data & CONTROL_TAG) != 0;
}

inline constexpr std::uint64_t make_control_cookie(std::uint64_t operation_cookie) noexcept {
    return CONTROL_TAG | operation_cookie;
}

inline constexpr std::uint64_t control_target_cookie(std::uint64_t user_data) noexcept {
    return user_data & COOKIE_MASK;
}

inline void bump(sluice::AsyncStats* s, std::uint64_t sluice::AsyncStats::* field) {
    if (s)
        ++(s->*field);
}

inline constexpr bool terminal_is_kernel_cancel(const IoError& error) noexcept {
    return error.code == IoError::Code::canceled || error.os_errno == ECANCELED;
}

#if defined(SLUICE_E1_MUTANT_STICKY_INTENT_DROPPED)
inline constexpr bool sticky_intent_retained() noexcept {
    return false;
}
#else
inline constexpr bool sticky_intent_retained() noexcept {
    return true;
}
#endif

inline constexpr bool cookie_terminal_is_canceled(const detail::TerminalResult& t) noexcept {
    return t.stored && t.is_error && terminal_is_kernel_cancel(t.error);
}

class UringAsyncBackend::BoundedDispatchQueue {
  public:
    explicit BoundedDispatchQueue(std::size_t capacity) : storage_(capacity), capacity_(capacity) {}
    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    detail::SlotHandle front() const noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: ring "
                                 "front() on empty queue (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[head_];
    }

    void push_back(detail::SlotHandle h) noexcept {
        if (size_ >= capacity_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: ring overflow "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] = h;
        ++size_;
    }
    bool pop_front(detail::SlotHandle& out) noexcept {
        if (size_ == 0)
            return false;
        out = storage_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }

    bool remove_exact(detail::SlotHandle h) noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            std::size_t idx = (head_ + i) % capacity_;
            if (storage_[idx].slot.value == h.slot.value &&
                storage_[idx].generation.value == h.generation.value) {
                for (std::size_t j = i; j + 1 < size_; ++j) {
                    std::size_t a = (head_ + j) % capacity_;
                    std::size_t b = (head_ + j + 1) % capacity_;
                    storage_[a] = storage_[b];
                }
                --size_;
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<detail::SlotHandle> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_;
};

class UringAsyncBackend::TransportLedger {
  public:
    enum class Kind : std::uint8_t { operation, cancel_control };

    struct Entry {
        std::uint64_t sequence = 0;
        std::uint32_t physical_position = 0;
        Kind kind = Kind::operation;
        std::uint64_t cookie = 0;
        detail::SlotHandle handle{};
        bool class_a_recovery_retired = false;
    };

    explicit TransportLedger(std::size_t capacity) : storage_(capacity), capacity_(capacity) {
        if (capacity_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: zero-capacity transport "
                                 "ledger (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
    }

    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    void append(Kind kind, std::uint32_t physical_position, std::uint64_t cookie,
                detail::SlotHandle handle) noexcept {
        const std::uint32_t expected_physical =
            last_sequence_ == 0
                ? physical_position
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(last_physical_position_) + 1u) % capacity_);
        if (size_ >= capacity_ || physical_position >= capacity_ || next_sequence_ == 0 ||
            next_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            physical_position != expected_physical || next_sequence_ != last_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "overflow/non-monotonic physical sequence/sequence exhaustion "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] =
            Entry{next_sequence_++, physical_position, kind, cookie, handle, false};
        last_sequence_ = next_sequence_ - 1u;
        last_physical_position_ = physical_position;
        ++size_;
    }

    Entry pop_front() noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "underflow (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        Entry out = storage_[head_];
        if (out.sequence != retired_prefix_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger retired a "
                                 "non-monotonic logical prefix (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        retired_prefix_sequence_ = out.sequence;
        head_ = (head_ + 1) % capacity_;
        --size_;
        return out;
    }

    Entry& at(std::size_t offset) noexcept {
        if (offset >= size_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger index "
                                 "out of range (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[(head_ + offset) % capacity_];
    }

    bool all_class_a_recovery_retired() const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!storage_[(head_ + i) % capacity_].class_a_recovery_retired)
                return false;
        }
        return true;
    }

  private:
    std::vector<Entry> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t retired_prefix_sequence_ = 0;
    std::uint32_t last_physical_position_ = 0;
};

}

#endif
