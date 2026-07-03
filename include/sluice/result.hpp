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

template <class T>
class Result;

namespace detail {

// Tag types to disambiguate the in-place success / error constructors.
struct success_tag {};
struct error_tag {};
struct unexpect_tag {};

template <class T>
struct result_storage {
    union {
        T value_;
    };
    IoError error_;
    bool has_value_;

    explicit result_storage(success_tag, T&& v) : value_(std::move(v)), has_value_(true) {}
    explicit result_storage(success_tag, const T& v) : value_(v), has_value_(true) {}
    explicit result_storage(error_tag, IoError e) : error_(e), has_value_(false) {}

    result_storage(const result_storage& o) : has_value_(o.has_value_) {
        if (has_value_) ::new (static_cast<void*>(std::addressof(value_))) T(o.value_);
        else error_ = o.error_;
    }
    result_storage(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(o.has_value_) {
        if (has_value_) ::new (static_cast<void*>(std::addressof(value_))) T(std::move(o.value_));
        else error_ = o.error_;
    }
    result_storage& operator=(const result_storage& o) {
        if (this != &o) {
            destroy();
            // Construct the new value BEFORE publishing has_value_ so that if
            // T's copy ctor throws, the object is left in a destroy-safe state
            // (has_value_ false, no live T) rather than claiming to hold a
            // value it never constructed.
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
    result_storage& operator=(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &o) {
            destroy();
            // See copy-assign: construct before publishing has_value_.
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

    void destroy() {
        if (has_value_) value_.~T();
    }
};

template <>
struct result_storage<void> {
    IoError error_;
    bool has_value_;
    explicit result_storage(success_tag) : error_{}, has_value_(true) {}
    explicit result_storage(error_tag, IoError e) : error_(e), has_value_(false) {}
};

}  // namespace detail

// Helper to construct the error variant explicitly: make_unexpected<T>(err).
template <class T>
Result<T> make_unexpected(IoError e) {
    return Result<T>(typename detail::error_tag{}, e);
}
Result<void> make_unexpected_void(IoError e);  // defined below, after Result<void>

template <class T>
class [[nodiscard]] Result {
public:
    Result(T&& v) : storage_(typename detail::success_tag{}, std::move(v)) {}          // NOLINT
    Result(const T& v) : storage_(typename detail::success_tag{}, v) {}                // NOLINT
    Result(typename detail::error_tag, IoError e) : storage_(typename detail::error_tag{}, e) {}

    Result(const Result&) = default;
    Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

    bool has_value() const noexcept { return storage_.has_value_; }
    explicit operator bool() const noexcept { return has_value(); }

    const T& value() const& { return storage_.value_; }
    T& value() & { return storage_.value_; }
    T&& value() && { return std::move(storage_.value_); }

    const IoError& error() const& { return storage_.error_; }

    T value_or(T fallback) const& {
        return has_value() ? storage_.value_ : fallback;
    }
    T value_or(T fallback) && {
        return has_value() ? std::move(storage_.value_) : std::move(fallback);
    }

private:
    friend Result<T> make_unexpected<T>(IoError);
    detail::result_storage<T> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() : storage_(typename detail::success_tag{}) {}                              // NOLINT
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

}  // namespace sluice
