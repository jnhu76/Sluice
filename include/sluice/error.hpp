#pragma once

#include <cerrno>
#include <cstdint>
#include <string_view>

namespace sluice {

struct IoError {
    enum class Code : std::uint8_t {
        eof,
        canceled,
        interrupted,
        would_block,
        no_space,
        permission_denied,
        invalid_state,
        backend_error,

        invalid_argument,
        not_found,
        not_supported,
    };

    Code code;
    int os_errno = 0;

    friend bool operator==(const IoError&, const IoError&) noexcept = default;
};

inline constexpr std::string_view to_string(IoError::Code c) {
    switch (c) {
    case IoError::Code::eof:
        return "eof";
    case IoError::Code::canceled:
        return "canceled";
    case IoError::Code::interrupted:
        return "interrupted";
    case IoError::Code::would_block:
        return "would_block";
    case IoError::Code::no_space:
        return "no_space";
    case IoError::Code::permission_denied:
        return "permission_denied";
    case IoError::Code::invalid_state:
        return "invalid_state";
    case IoError::Code::backend_error:
        return "backend_error";
    case IoError::Code::invalid_argument:
        return "invalid_argument";
    case IoError::Code::not_found:
        return "not_found";
    case IoError::Code::not_supported:
        return "not_supported";
    }
    return "unknown";
}

namespace detail {

// The single errno-to-category table; native-error sites route through it
// instead of classifying errno locally. A native error with no row keeps
// `backend_error` plus its native detail. ECANCELED is deliberately unmapped:
// the semantic `canceled` outcome comes from cancellation dispositions, not
// from an errno.
struct NativeErrorMapping {
    int native_errno;
    IoError::Code canonical;
};

inline constexpr NativeErrorMapping kNativeErrorMappings[] = {
    {EACCES, IoError::Code::permission_denied},
    {EPERM, IoError::Code::permission_denied},
    {ENOENT, IoError::Code::not_found},
    {ENOTDIR, IoError::Code::not_found},
    {ENOSPC, IoError::Code::no_space},
    {EDQUOT, IoError::Code::no_space},
    {EINTR, IoError::Code::interrupted},
    {EAGAIN, IoError::Code::would_block},
#if EWOULDBLOCK != EAGAIN
    {EWOULDBLOCK, IoError::Code::would_block},
#endif
};

inline constexpr IoError::Code canonical_error_code(int native_errno) noexcept {
    // errno == 0 means a call reported failure without setting errno: there is
    // no native cause to preserve, so no canonical category can be derived.
    if (native_errno == 0)
        return IoError::Code::backend_error;
    for (const NativeErrorMapping& mapping : kNativeErrorMappings) {
        if (mapping.native_errno == native_errno)
            return mapping.canonical;
    }
    return IoError::Code::backend_error;
}

} // namespace detail

inline IoError from_errno_value(int err) {
    return IoError{.code = detail::canonical_error_code(err), .os_errno = err};
}

} // namespace sluice
