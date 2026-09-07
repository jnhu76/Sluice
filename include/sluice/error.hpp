


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

}
