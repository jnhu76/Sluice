





#pragma once

#include <sluice/error.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace sluice {

template <class T> class Result;

namespace detail {


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



        if (o.has_value_) {
            ::new (static_cast<void*>(std::addressof(value_))) T(o.value_);
            has_value_ = true;
        } else {
            error_ = o.error_;
        }
    }
    result_storage(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(false) {


        if (o.has_value_) {
            ::new (static_cast<void*>(std::addressof(value_))) T(std::move(o.value_));
            has_value_ = true;
        } else {
            error_ = o.error_;
        }
    }
    result_storage& operator=(const result_storage& o) {
        if (this != &o) {








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





    result_storage&
    operator=(result_storage&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &o) {



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













    void destroy_and_clear() noexcept {
        if (has_value_) {
            value_.~T();
        }
        error_ = IoError{IoError::Code::invalid_state};
        has_value_ = false;
    }



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

}


template <class T> Result<T> make_unexpected(IoError e) {
    return Result<T>(typename detail::error_tag{}, e);
}
Result<void> make_unexpected_void(IoError e);

template <class T> class [[nodiscard]] Result {
  public:
    Result(T&& v) : storage_(typename detail::success_tag{}, std::move(v)) {} // NOLINT
    Result(const T& v) : storage_(typename detail::success_tag{}, v) {}       // NOLINT
    Result(typename detail::error_tag, IoError e) : storage_(typename detail::error_tag{}, e) {}

    Result(const Result&) = default;
    Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    Result& operator=(const Result&) = default;






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


inline Result<void> make_unexpected(IoError e) {
    return make_unexpected_void(e);
}

}
