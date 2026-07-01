// cppio::IoError — error model for the I/O core.
// Inspired by Zig std.Io's error set (ReadFailed/WriteFailed/EndOfStream) but
// flattened to a tagged code plus an OS errno slot for diagnostics.
#pragma once

#include <cstdint>
#include <string_view>

namespace cppio {

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
    };

    Code code;
    int os_errno = 0;

    friend bool operator==(const IoError&, const IoError&) = default;
};

// Stable string name for a code. Used for diagnostics only, not control flow.
inline constexpr std::string_view to_string(IoError::Code c) {
    switch (c) {
        case IoError::Code::eof: return "eof";
        case IoError::Code::canceled: return "canceled";
        case IoError::Code::interrupted: return "interrupted";
        case IoError::Code::would_block: return "would_block";
        case IoError::Code::no_space: return "no_space";
        case IoError::Code::permission_denied: return "permission_denied";
        case IoError::Code::invalid_state: return "invalid_state";
        case IoError::Code::backend_error: return "backend_error";
    }
    return "unknown";
}

// Maps a POSIX errno value to an IoError. Deterministic. errno 0 -> backend_error
// (it is never used to construct an error in normal flow; callers gate on err!=0).
inline IoError from_errno_value(int err) {
    IoError e{};
    e.os_errno = err;
    switch (err) {
        case 0:            e.code = IoError::Code::backend_error; break;
        case 13: /*EACCES*/ e.code = IoError::Code::permission_denied; break;
        case 28: /*ENOSPC*/ e.code = IoError::Code::no_space; break;
        case 4:  /*EINTR*/  e.code = IoError::Code::interrupted; break;
        case 11: /*EAGAIN*/ e.code = IoError::Code::would_block; break;
        case 125: /*ECANCELED*/ e.code = IoError::Code::canceled; break;
        default:            e.code = IoError::Code::backend_error; break;
    }
    return e;
}

}  // namespace cppio
