// sluice::Result<T> — a minimal expected-like type for error propagation.
//
// Carries either a value of type T or a sluice::IoError. Result<void> carries
// only success-or-error. Kept deliberately small: no monadic ops beyond what
// the core needs (has_value, value, error, value_or, move-out). We avoid a
// third-party dependency for std::expected by staying on C++20.
#pragma once

#include <sluice/error.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace sluice {

template <class T> class Result;

namespace detail {

// Tag types to disambiguate the in-place success / error constructors.
struct success_tag {};
struct error_tag {};
struct unexpect_tag {};

template <class T> struct result_storage {
    union {
        T value_;
    };
    IoError error_;
    bool has_value_;

    explicit result_storage(success_tag, T&& v) : value_(std::move(v)), has_value_(true) {}
    explicit result_storage(success_tag, const T& v) : value_(v), has_value_(true) {}
    explicit result_storage(error_tag, IoError e) : error_(e), has_value_(false) {}

    result_storage(const result_storage& o) : has_value_(false) {
        // E15-P1-01: initialize has_value_ to false so that if T's copy ctor
        // below throws, the discriminator is already false and the unwind's
        // ~result_storage() does NOT run ~T() on never-constructed storage.
        if (o.has_value_) {
            ::new (static_cast<void*>(std::addressof(value_))) T(o.value_);
            has_value_ = true;
        } else {
            error_ = o.error_;
        }
    }
    result_storage(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(false) {
        // E15-P1-01: see copy ctor — construct first, publish has_value_ last.
        if (o.has_value_) {
            ::new (static_cast<void*>(std::addressof(value_))) T(std::move(o.value_));
            has_value_ = true;
        } else {
            error_ = o.error_;
        }
    }
    result_storage& operator=(const result_storage& o) {
        if (this != &o) {
            // E15-P1-01: clear the discriminator BEFORE doing anything else so
            // the storage is in a destroy-safe state (no live T, has_value_
            // false) for the duration of the transition. destroy() runs the
            // old ~T() (if any); then if the placement-new copy construction
            // below throws, the discriminator is already false and the
            // eventual ~result_storage() does NOT run ~T() again on dead
            // storage. Only on a successful construction do we publish
            // has_value_ = true.
            destroy_and_clear();
            if (o.has_value_) {
                ::new (static_cast<void*>(std::addressof(value_))) T(o.value_);
                has_value_ = true;
            } else {
                error_ = o.error_;
                has_value_ = false;
            }
        }
        return *this;
    }
    // E15-P1-02: this operator performs placement-new move CONSTRUCTION (never
    // T::operator=); its noexcept must therefore track
    // is_nothrow_move_CONSTRUCTIBLE_v<T>, NOT is_nothrow_move_assignable_v<T>.
    // (The public Result<T>::operator=(Result&&) below forwards here and is
    // declared with the matching condition.)
    result_storage&
    operator=(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &o) {
            // E15-P1-01: see copy-assign — clear first, construct second,
            // publish last. A throwing move construction leaves has_value_
            // false and the storage destroy-safe.
            destroy_and_clear();
            if (o.has_value_) {
                ::new (static_cast<void*>(std::addressof(value_))) T(std::move(o.value_));
                has_value_ = true;
            } else {
                error_ = o.error_;
                has_value_ = false;
            }
        }
        return *this;
    }
    ~result_storage() { destroy(); }

    // Tear down the live value (if any) AND reset the discriminator. E15-P1-01:
    // the reset must happen here so that any subsequent throwing construction
    // cannot leave the discriminator claiming a value is live when it is not.
    void destroy_and_clear() {
        if (has_value_) {
            value_.~T();
            has_value_ = false;
        }
    }
    // Destroy the live value without touching the discriminator. Used only by
    // ~result_storage() (where the whole object is going away) and by legacy
    // call sites that publish has_value_ themselves.
    void destroy() {
        if (has_value_) {
            value_.~T();
        }
    }
};

template <> struct result_storage<void> {
    IoError error_;
    bool has_value_;
    explicit result_storage(success_tag) : error_{}, has_value_(true) {}
    explicit result_storage(error_tag, IoError e) : error_(e), has_value_(false) {}

    result_storage(const result_storage&) = default;
    result_storage(result_storage&&) noexcept = default;
    result_storage& operator=(const result_storage&) = default;
    result_storage& operator=(result_storage&&) noexcept = default;
    ~result_storage() = default;

    friend bool operator==(const result_storage&, const result_storage&) = default;
};

} // namespace detail

// Helper to construct the error variant explicitly: make_unexpected<T>(err).
template <class T> Result<T> make_unexpected(IoError e) {
    return Result<T>(typename detail::error_tag{}, e);
}
Result<void> make_unexpected_void(IoError e); // defined below, after Result<void>

template <class T> class [[nodiscard]] Result {
  public:
    Result(T&& v) : storage_(typename detail::success_tag{}, std::move(v)) {} // NOLINT
    Result(const T& v) : storage_(typename detail::success_tag{}, v) {}       // NOLINT
    Result(typename detail::error_tag, IoError e) : storage_(typename detail::error_tag{}, e) {}

    Result(const Result&) = default;
    Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    Result& operator=(const Result&) = default;
    // E15-P1-02: the storage move-assign does placement-new move CONSTRUCTION
    // (never T::operator=), so its noexcept is governed by
    // is_nothrow_move_constructible_v<T>. The previous condition
    // (is_nothrow_move_assignable_v<T>) was a different trait and could
    // advertise noexcept(true) for a function whose body actually throws,
    // causing std::terminate.
    Result& operator=(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;

    bool has_value() const noexcept { return storage_.has_value_; }
    explicit operator bool() const noexcept { return has_value(); }

    const T& value() const& { return storage_.value_; }
    T& value() & { return storage_.value_; }
    T&& value() && { return std::move(storage_.value_); }

    const IoError& error() const& { return storage_.error_; }

    T value_or(T fallback) const& { return has_value() ? storage_.value_ : fallback; }
    T value_or(T fallback) && {
        return has_value() ? std::move(storage_.value_) : std::move(fallback);
    }

  private:
    friend Result<T> make_unexpected<T>(IoError);
    detail::result_storage<T> storage_;
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() : storage_(typename detail::success_tag{}) {} // NOLINT
    Result(typename detail::error_tag, IoError e) : storage_(typename detail::error_tag{}, e) {}

    bool has_value() const noexcept { return storage_.has_value_; }
    explicit operator bool() const noexcept { return has_value(); }

    const IoError& error() const& { return storage_.error_; }

  private:
    friend Result<void> make_unexpected_void(IoError);
    detail::result_storage<void> storage_;
};

inline Result<void> make_unexpected_void(IoError e) {
    return Result<void>(typename detail::error_tag{}, e);
}

// Convenience overload for the void specialization.
inline Result<void> make_unexpected(IoError e) {
    return make_unexpected_void(e);
}

} // namespace sluice
