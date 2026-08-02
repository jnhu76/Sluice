// sluice::IoError — error model for the I/O core.
// Inspired by Zig std.Io's error set (ReadFailed/WriteFailed/EndOfStream) but
// flattened to a tagged code plus an OS errno slot for diagnostics.
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
        // ADR-explicit-io-request-contract (Accepted) Decision 6: the request lifecycle
        // distinguishes admission rejection, stale-key lookup, and capability refusal from
        // one another and from configured-capacity would_block, lifecycle invalid_state,
        // and genuine-init no_space. These three codes name those distinct cases.
        //   invalid_argument — malformed operation descriptor (prepare-stage rejection)
        //   not_found        — stale/unknown RequestKey (cancel/reap lookup after release)
        //   not_supported    — backend/platform does not provide the op or cancel capability
        invalid_argument,
        not_found,
        not_supported,
    };

    Code code;
    int os_errno = 0;

    friend bool operator==(const IoError&, const IoError&) noexcept = default;
};

// Stable string name for a code. Used for diagnostics only, not control flow.
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

// Maps a POSIX errno value to an IoError. Uses portable <cerrno> macros (never
// hardcoded ints) so behavior is stable across Linux/macOS/BSD. os_errno is
// always preserved verbatim; errno 0 maps to backend_error since it is never a
// real error (callers gate on err != 0).
inline IoError from_errno_value(int err) {
    IoError e{};
    e.os_errno = err;
    switch (err) {
    case 0:
        e.code = IoError::Code::backend_error;
        break;
    case EACCES:
    case EPERM:
    case ENOENT:
    case ENOTDIR:
        e.code = IoError::Code::permission_denied;
        break;
    case ENOSPC:
    case EDQUOT:
        e.code = IoError::Code::no_space;
        break;
    case EINTR:
        e.code = IoError::Code::interrupted;
        break;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        e.code = IoError::Code::would_block;
        break;
#ifdef ECANCELED
    case ECANCELED:
        e.code = IoError::Code::canceled;
        break;
#endif
    default:
        e.code = IoError::Code::backend_error;
        break;
    }
    return e;
}

} // namespace sluice
